#ifndef JOIN_OP_NODE_H
#define JOIN_OP_NODE_H

#include "RelAlgNode.h"
#include "BinaryOp.h"
#include "../visitor/Visitor.h"

class JoinOp : public BinaryOp {
    
public:
	Predicate* pred;

	explicit JoinOp(RelExpr *n1, RelExpr *n2, Predicate* n3) : relex1(n1), relex2(n2), pred(n3) {}
	
    /**< Accepts the given void visitor by calling v.visit(this) */
    virtual void accept(class Visitor &v) = 0;
};

#endif // JOIN_OP_NODE_H