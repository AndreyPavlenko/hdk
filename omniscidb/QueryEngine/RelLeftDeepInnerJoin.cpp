/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RelLeftDeepInnerJoin.h"
#include "IR/ExprRewriter.h"
#include "Logger/Logger.h"
#include "RelAlgDagBuilder.h"

#include <numeric>

namespace {

void collect_left_deep_join_inputs(
    std::deque<std::shared_ptr<const RelAlgNode>>& inputs,
    std::vector<std::shared_ptr<const RelJoin>>& original_joins,
    const std::shared_ptr<const RelJoin>& join) {
  original_joins.push_back(join);
  CHECK_EQ(size_t(2), join->inputCount());
  const auto left_input_join =
      std::dynamic_pointer_cast<const RelJoin>(join->getAndOwnInput(0));
  if (left_input_join) {
    inputs.push_front(join->getAndOwnInput(1));
    collect_left_deep_join_inputs(inputs, original_joins, left_input_join);
  } else {
    inputs.push_front(join->getAndOwnInput(1));
    inputs.push_front(join->getAndOwnInput(0));
  }
}

std::pair<std::shared_ptr<RelLeftDeepInnerJoin>, std::shared_ptr<const RelAlgNode>>
create_left_deep_join(const std::shared_ptr<RelAlgNode>& left_deep_join_root) {
  const auto old_root = get_left_deep_join_root(left_deep_join_root);
  if (!old_root) {
    return {nullptr, nullptr};
  }
  std::deque<std::shared_ptr<const RelAlgNode>> inputs_deque;
  const auto left_deep_join_filter =
      std::dynamic_pointer_cast<RelFilter>(left_deep_join_root);
  const auto join =
      std::dynamic_pointer_cast<const RelJoin>(left_deep_join_root->getAndOwnInput(0));
  CHECK(join);
  std::vector<std::shared_ptr<const RelJoin>> original_joins;
  collect_left_deep_join_inputs(inputs_deque, original_joins, join);
  std::vector<std::shared_ptr<const RelAlgNode>> inputs(inputs_deque.begin(),
                                                        inputs_deque.end());
  return {std::make_shared<RelLeftDeepInnerJoin>(
              left_deep_join_filter, inputs, original_joins),
          old_root};
}

class RebindInputsFromLeftDeepJoinVisitor : public hdk::ir::ExprRewriter {
 public:
  RebindInputsFromLeftDeepJoinVisitor(const RelLeftDeepInnerJoin* left_deep_join)
      : left_deep_join_(left_deep_join) {
    std::vector<size_t> input_sizes;
    input_sizes.reserve(left_deep_join->inputCount());
    CHECK_GT(left_deep_join->inputCount(), size_t(1));
    for (size_t i = 0; i < left_deep_join->inputCount(); ++i) {
      input_sizes.push_back(left_deep_join->getInput(i)->size());
    }
    input_size_prefix_sums_.resize(input_sizes.size());
    std::partial_sum(
        input_sizes.begin(), input_sizes.end(), input_size_prefix_sums_.begin());
  }

  hdk::ir::ExprPtr visitColumnRef(const hdk::ir::ColumnRef* col_ref) override {
    const auto node = col_ref->node();
    if (left_deep_join_->coversOriginalNode(node)) {
      const auto it = std::lower_bound(input_size_prefix_sums_.begin(),
                                       input_size_prefix_sums_.end(),
                                       col_ref->index(),
                                       std::less_equal<size_t>());
      CHECK(it != input_size_prefix_sums_.end());
      auto new_node =
          left_deep_join_->getInput(std::distance(input_size_prefix_sums_.begin(), it));
      auto new_index = col_ref->index();
      if (it != input_size_prefix_sums_.begin()) {
        const auto prev_input_count = *(it - 1);
        CHECK_LE(prev_input_count, new_index);
        new_index -= prev_input_count;
      }
      return hdk::ir::makeExpr<hdk::ir::ColumnRef>(col_ref->type(), new_node, new_index);
    }
    return defaultResult(col_ref);
  };

 private:
  std::vector<size_t> input_size_prefix_sums_;
  const RelLeftDeepInnerJoin* left_deep_join_;
};

}  // namespace

RelLeftDeepInnerJoin::RelLeftDeepInnerJoin(
    const std::shared_ptr<RelFilter>& filter,
    std::vector<std::shared_ptr<const RelAlgNode>> inputs,
    std::vector<std::shared_ptr<const RelJoin>>& original_joins)
    : RelAlgNode(inputs)
    , condition_(filter ? filter->getConditionExprShared() : nullptr)
    , original_filter_(filter)
    , original_joins_(original_joins) {
  // Accumulate join conditions from the (explicit) joins themselves and
  // from the filter node at the root of the left-deep tree pattern.
  outer_conditions_per_level_.resize(original_joins.size());
  for (size_t nesting_level = 0; nesting_level < original_joins.size(); ++nesting_level) {
    const auto& original_join = original_joins[nesting_level];
    const auto condition_true =
        dynamic_cast<const hdk::ir::Constant*>(original_join->getCondition());

    bool cond_is_not_const_true = !condition_true ||
                                  !condition_true->type()->isBoolean() ||
                                  !condition_true->value().boolval;
    if (cond_is_not_const_true) {
      switch (original_join->getJoinType()) {
        case JoinType::INNER:
        case JoinType::SEMI:
        case JoinType::ANTI: {
          if (original_join->getCondition()) {
            if (!condition_) {
              condition_ = original_join->getConditionShared();
            } else {
              condition_ = hdk::ir::makeExpr<hdk::ir::BinOper>(
                  condition_->ctx().boolean(),
                  hdk::ir::OpType::kAnd,
                  kONE,
                  condition_,
                  original_join->getConditionShared());
            }
          }
          break;
        }
        case JoinType::LEFT: {
          if (original_join->getCondition()) {
            outer_conditions_per_level_[nesting_level] =
                rebind_inputs_from_left_deep_join(original_join->getCondition(), this);
          }
          break;
        }
        default:
          CHECK(false);
      }
    }
  }

  if (condition_) {
    condition_ = rebind_inputs_from_left_deep_join(condition_.get(), this);
  } else {
    condition_ = hdk::ir::Constant::make(hdk::ir::Context::defaultCtx().boolean(), true);
  }
}

const hdk::ir::Expr* RelLeftDeepInnerJoin::getInnerCondition() const {
  return condition_.get();
}
hdk::ir::ExprPtr RelLeftDeepInnerJoin::getInnerConditionShared() const {
  return condition_;
}

const hdk::ir::Expr* RelLeftDeepInnerJoin::getOuterCondition(
    const size_t nesting_level) const {
  CHECK_GE(nesting_level, size_t(1));
  CHECK_LE(nesting_level, outer_conditions_per_level_.size());
  // Outer join conditions are collected depth-first while the returned condition
  // must be consistent with the order of the loops (which is reverse depth-first).
  return outer_conditions_per_level_[outer_conditions_per_level_.size() - nesting_level]
      .get();
}
hdk::ir::ExprPtr RelLeftDeepInnerJoin::getOuterConditionShared(
    const size_t nesting_level) const {
  CHECK_GE(nesting_level, size_t(1));
  CHECK_LE(nesting_level, outer_conditions_per_level_.size());
  // Outer join conditions are collected depth-first while the returned condition
  // must be consistent with the order of the loops (which is reverse depth-first).
  return outer_conditions_per_level_[outer_conditions_per_level_.size() - nesting_level];
}

const JoinType RelLeftDeepInnerJoin::getJoinType(const size_t nesting_level) const {
  CHECK_LE(nesting_level, original_joins_.size());
  return original_joins_[original_joins_.size() - nesting_level]->getJoinType();
}

std::string RelLeftDeepInnerJoin::toString() const {
  std::stringstream ss;
  ss << ::typeName(this) << getIdString() << "(cond=" << ::toString(condition_)
     << ", outer=" << ::toString(outer_conditions_per_level_)
     << ", input=" << inputsToString(inputs_) << ")";
  return ss.str();
}

size_t RelLeftDeepInnerJoin::toHash() const {
  if (!hash_) {
    hash_ = typeid(RelLeftDeepInnerJoin).hash_code();
    boost::hash_combine(*hash_, condition_ ? condition_->hash() : boost::hash_value("n"));
    for (auto& node : inputs_) {
      boost::hash_combine(*hash_, node->toHash());
    }
  }
  return *hash_;
}

size_t RelLeftDeepInnerJoin::size() const {
  size_t total_size = 0;
  for (const auto& input : inputs_) {
    total_size += input->size();
  }
  return total_size;
}

std::shared_ptr<RelAlgNode> RelLeftDeepInnerJoin::deepCopy() const {
  CHECK(false);
  return nullptr;
}

bool RelLeftDeepInnerJoin::coversOriginalNode(const RelAlgNode* node) const {
  if (node == original_filter_.get()) {
    return true;
  }
  for (const auto& original_join : original_joins_) {
    if (original_join.get() == node) {
      return true;
    }
  }
  return false;
}

const RelFilter* RelLeftDeepInnerJoin::getOriginalFilter() const {
  return original_filter_.get();
}

std::vector<std::shared_ptr<const RelJoin>> RelLeftDeepInnerJoin::getOriginalJoins()
    const {
  std::vector<std::shared_ptr<const RelJoin>> original_joins;
  original_joins.assign(original_joins_.begin(), original_joins_.end());
  return original_joins;
}

// Recognize the left-deep join tree pattern with an optional filter as root
// with `node` as the parent of the join sub-tree. On match, return the root
// of the recognized tree (either the filter node or the outermost join).
std::shared_ptr<const RelAlgNode> get_left_deep_join_root(
    const std::shared_ptr<RelAlgNode>& node) {
  const auto left_deep_join_filter = dynamic_cast<const RelFilter*>(node.get());
  if (left_deep_join_filter) {
    const auto join = dynamic_cast<const RelJoin*>(left_deep_join_filter->getInput(0));
    if (!join) {
      return nullptr;
    }
    if (join->getJoinType() == JoinType::INNER || join->getJoinType() == JoinType::SEMI ||
        join->getJoinType() == JoinType::ANTI) {
      return node;
    }
  }
  if (!node || node->inputCount() != 1) {
    return nullptr;
  }
  const auto join = dynamic_cast<const RelJoin*>(node->getInput(0));
  if (!join) {
    return nullptr;
  }
  return node->getAndOwnInput(0);
}

hdk::ir::ExprPtr rebind_inputs_from_left_deep_join(
    const hdk::ir::Expr* expr,
    const RelLeftDeepInnerJoin* left_deep_join) {
  RebindInputsFromLeftDeepJoinVisitor visitor(left_deep_join);
  return visitor.visit(expr);
}

void create_left_deep_join(std::vector<std::shared_ptr<RelAlgNode>>& nodes) {
  std::list<std::shared_ptr<RelAlgNode>> new_nodes;
  for (auto& left_deep_join_candidate : nodes) {
    std::shared_ptr<RelLeftDeepInnerJoin> left_deep_join;
    std::shared_ptr<const RelAlgNode> old_root;
    std::tie(left_deep_join, old_root) = create_left_deep_join(left_deep_join_candidate);
    if (!left_deep_join) {
      continue;
    }
    CHECK_GE(left_deep_join->inputCount(), size_t(2));
    for (auto& node : nodes) {
      if (node && node->hasInput(old_root.get())) {
        node->replaceInput(left_deep_join_candidate, left_deep_join);
        std::shared_ptr<const RelJoin> old_join;
        if (std::dynamic_pointer_cast<const RelJoin>(left_deep_join_candidate)) {
          old_join = std::static_pointer_cast<const RelJoin>(left_deep_join_candidate);
        } else {
          CHECK_EQ(size_t(1), left_deep_join_candidate->inputCount());
          old_join = std::dynamic_pointer_cast<const RelJoin>(
              left_deep_join_candidate->getAndOwnInput(0));
        }
        while (old_join) {
          node->replaceInput(old_join, left_deep_join);
          old_join =
              std::dynamic_pointer_cast<const RelJoin>(old_join->getAndOwnInput(0));
        }
      }
    }

    new_nodes.emplace_back(std::move(left_deep_join));
  }

  // insert the new left join nodes to the front of the owned RelAlgNode list.
  // This is done to ensure all created RelAlgNodes exist in this list for later
  // visitation, such as RelAlgDagBuilder::resetQueryExecutionState.
  nodes.insert(nodes.begin(), new_nodes.begin(), new_nodes.end());
}
