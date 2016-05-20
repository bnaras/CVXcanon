
#include "LinearConeTransform.hpp"

#include <unordered_map>

#include <glog/logging.h>

#include "cvxcanon/expression/Expression.hpp"
#include "cvxcanon/expression/ExpressionShape.hpp"
#include "cvxcanon/expression/ExpressionUtil.hpp"
#include "cvxcanon/expression/TextFormat.hpp"

typedef Expression(*TransformFunction)(
    const Expression& expr,
    std::vector<Expression>* constraints);

Expression transform_abs(
    const Expression& expr,
    std::vector<Expression>* constraints) {
  const Expression& x = expr.arg(0);
  Expression t = epi_var(expr, "abs");
  constraints->push_back(leq(x, t));
  constraints->push_back(leq(neg(x), t));
  return t;
}

Expression transform_p_norm(
    const Expression& expr,
    std::vector<Expression>* constraints) {
  CHECK_EQ(expr.attr<PNormAttributes>().p, 1);
  return sum_entries(transform_abs(expr, constraints));
}

Expression transform_quad_over_lin(
    const Expression& expr,
    std::vector<Expression>* constraints) {
  const Expression& x = expr.arg(0);
  const Expression& y = expr.arg(1);
  Expression t = scalar_epi_var(expr, "qol");

  constraints->push_back(
      soc(vstack({add(y, neg(t)), mul(constant(2), x)}), add(y, t)));
  constraints->push_back(leq(constant(0), y));
  return t;
}

std::unordered_map<int, TransformFunction> kTransforms = {
  {Expression::ABS, &transform_abs},
  {Expression::P_NORM, &transform_p_norm},
  {Expression::QUAD_OVER_LIN, &transform_quad_over_lin},
};

Expression transform_expression(
    const Expression& expr,
    std::vector<Expression>* constraints) {
  // First transform children
  std::vector<Expression> linear_args;
  for (const Expression& arg : expr.args())
    linear_args.push_back(transform_expression(arg, constraints));

  // New expression, with linear args
  Expression output = Expression(expr.type(), linear_args, expr.attr_ptr());

  // Now transform non-linear functions, if necessary
  VLOG(2) << "transform_func: " << format_expression(expr);
  auto iter = kTransforms.find(expr.type());
  if (iter != kTransforms.end())
    output = iter->second(output, constraints);

  return output;
}

Problem LinearConeTransform::transform(const Problem& problem) {
  std::vector<Expression> constraints;
  Expression linear_objective = transform_expression(
      problem.objective, &constraints);
  for (const Expression& constr : problem.constraints) {
    constraints.push_back(
        transform_expression(constr, &constraints));
  }
  return {problem.sense, linear_objective, constraints};
}
