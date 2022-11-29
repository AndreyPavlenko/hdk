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
#ifndef QUERYENGINE_EQUIJOINCONDITION_H
#define QUERYENGINE_EQUIJOINCONDITION_H

#include <list>
#include <memory>

namespace hdk::ir {
class BinOper;
class Expr;
}  // namespace hdk::ir

// Go through the qualifiers and group consecutive equality operators to create
// a list of composite join conditions.
std::list<hdk::ir::ExprPtr> combine_equi_join_conditions(
    const std::list<hdk::ir::ExprPtr>& join_quals);

std::list<hdk::ir::ExprPtr> coalesce_singleton_equi_join(
    const std::shared_ptr<const hdk::ir::BinOper>& join_qual);

#endif  // QUERYENGINE_EQUIJOINCONDITION_H
