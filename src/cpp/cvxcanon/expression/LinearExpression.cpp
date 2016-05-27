#include "LinearExpression.hpp"

#include <unordered_map>
#include <map>
#include <vector>

#include "cvxcanon/expression/ExpressionShape.hpp"
#include "cvxcanon/expression/ExpressionUtil.hpp"
#include "cvxcanon/expression/TextFormat.hpp"
#include "cvxcanon/util/MatrixUtil.hpp"
#include "glog/logging.h"

bool is_constant(const CoeffMap& coeffs) {
  return (coeffs.find(kConstCoefficientId) != coeffs.end() &&
          coeffs.size() == 1);
}

std::vector<SparseMatrix> get_add_coefficients(const Expression& expr) {
  std::vector<SparseMatrix> coeffs;
  for (const Expression& arg : expr.args()) {
    // Handle promotion
    coeffs.push_back(
        dim(arg) == 1 ? ones_matrix(dim(expr), 1) : identity(dim(expr)));
  }
  return coeffs;
}

std::vector<SparseMatrix> get_left_mul_coefficients(
    const Expression& expr, const SparseMatrix& block) {
  const int block_rows = block.rows();
  const int block_cols = block.cols();
  const int num_blocks = size(expr).dims[1];

  Matrix coeffs (num_blocks * block_rows, num_blocks * block_cols);
  std::vector<Triplet> tripletList;
  tripletList.reserve(num_blocks * block.nonZeros());
  for (int curr_block = 0; curr_block < num_blocks; curr_block++) {
    int start_i = curr_block * block_rows;
    int start_j = curr_block * block_cols;
    for ( int k = 0; k < block.outerSize(); ++k ) {
      for ( Matrix::InnerIterator it(block, k); it; ++it ) {
        tripletList.push_back(Triplet(start_i + it.row(), start_j + it.col(),
                                      it.value()));
      }
    }
  }
  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

std::vector<SparseMatrix> get_right_mul_coefficients(
    const Expression& expr, const SparseMatrix& constant) {
  const int rows = constant.rows();
  const int cols = constant.cols();
  const int n = size(expr).dims[0];

  Matrix coeffs(cols * n, rows * n);
  std::vector<Triplet> tripletList;
  tripletList.reserve(n * constant.nonZeros());
  for ( int k = 0; k < constant.outerSize(); ++k ) {
    for ( Matrix::InnerIterator it(constant, k); it; ++it ) {
      double val = it.value();

      // each element of CONSTANT occupies an N x N block in the matrix
      int row_start = it.col() * n;
      int col_start = it.row() * n;
      for (int i = 0; i < n; i++) {
        int row_idx = row_start + i;
        int col_idx = col_start + i;
        tripletList.push_back(Triplet(row_idx, col_idx, val));
      }
    }
  }
  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

std::vector<SparseMatrix> get_neg_coefficients(const Expression& expr) {
  return {scalar_matrix(-1, dim(expr))};
}

std::vector<SparseMatrix> get_sum_entries_coefficients(const Expression& expr) {
  return {ones_matrix(1, dim(expr.arg(0)))};
}

std::vector<SparseMatrix> get_stack_coefficients(
    const Expression& expr, bool vertical) {
  std::vector<SparseMatrix> coeffs;
  int offset = 0;
  Size expr_size = size(expr);
  for (const Expression& arg : expr.args()) {
    Size arg_size = size(arg);

    /* If VERTICAL, columns that are interleaved. Otherwise, they are
       laid out in order. */
    int column_offset;
    int offset_increment;
    if (vertical) {
      column_offset = expr_size.dims[0];
      offset_increment = arg_size.dims[0];
    } else {
      column_offset = arg_size.dims[0];
      offset_increment = dim(arg);
    }

    std::vector<Triplet> arg_coeffs;
    arg_coeffs.reserve(dim(arg));
    for (int i = 0; i < arg_size.dims[0]; i++) {
      for (int j = 0; j < arg_size.dims[1]; j++) {
        int row_idx = i + (j * column_offset) + offset;
        int col_idx = i + (j * arg_size.dims[0]);
        arg_coeffs.push_back(Triplet(row_idx, col_idx, 1));
      }
    }

    coeffs.push_back(sparse_matrix(dim(expr), dim(arg), arg_coeffs));
    offset += offset_increment;
  }
  return coeffs;
}

std::vector<SparseMatrix> get_hstack_coefficients(const Expression& expr) {
  return get_stack_coefficients(expr, false);
}

std::vector<SparseMatrix> get_vstack_coefficients(const Expression& expr) {
  return get_stack_coefficients(expr, true);
}

std::vector<SparseMatrix> get_reshape_coefficients(const Expression& expr) {
  return {identity(dim(expr))};
}

std::vector<SparseMatrix> get_index_coefficients(const Expression& expr) {
  const int rows = size(expr.arg(0)).dims[0];
  const int cols = size(expr.arg(0)).dims[1];
  SparseMatrix coeffs(dim(expr), rows * cols);

  /* If slice is empty, return empty matrix */
  if (coeffs.rows() == 0 ||  coeffs.cols() == 0) {
    return {coeffs};
  }

  /* Row Slice Data */
  const Slice& rs = expr.attr<IndexAttributes>().keys[0];
  int row_start = rs.start < 0 ? rows + rs.start : rs.start;
  int row_stop = rs.stop < 0 ? rows + rs.stop : rs.stop;
  int row_step = rs.step;

  /* Column Slice Data */
  const Slice& cs = expr.attr<IndexAttributes>().keys[1];
  int col_start = cs.start < 0 ? cols + cs.start : cs.start;
  int col_stop = cs.stop < 0 ? cols + cs.stop : cs.stop;
  int col_step = cs.step;

  /* Set the index coefficients by looping over the column selection
   * first to remain consistent with CVXPY. */
  std::vector<Triplet> tripletList;
  int col = col_start;
  int counter = 0;
  while (true) {
    if (col < 0 || col >= cols) {
      break;
    }
    int row = row_start;
    while (true) {
      if (row < 0 || row >= rows) {
        break;
      }
      int row_idx = counter;
      int col_idx = col * rows + row;
      tripletList.push_back(Triplet(row_idx, col_idx, 1.0));
      counter++;
      row += row_step;
      if ((row_step > 0 && row >= row_stop) ||
          (row_step < 0 && row < row_stop)) {
        break;
      }
    }
    col += col_step;
    if ((col_step > 0 && col >= col_stop) ||
        (col_step < 0 && col < col_stop)) {
      break;
    }
  }
  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

std::vector<SparseMatrix> get_diag_mat_coefficients(const Expression& expr) {
  const int rows = size(expr).dims[0];

  SparseMatrix coeffs(rows, rows * rows);
  std::vector<Triplet> tripletList;
  tripletList.reserve(rows);
  for (int i = 0; i < rows; i++) {
    // index in the extracted vector
    int row_idx = i;
    // index in the original matrix
    int col_idx = i * rows + i;
    tripletList.push_back(Triplet(row_idx, col_idx, 1.0));
  }

  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

std::vector<SparseMatrix> get_diag_vec_coefficients(const Expression& expr) {
  const int rows = size(expr).dims[0];

  SparseMatrix coeffs(rows * rows, rows);
  std::vector<Triplet> tripletList;
  tripletList.reserve(rows);
  for (int i = 0; i < rows; i++) {
    // index in the diagonal matrix
    int row_idx = i * rows + i;
    // index in the original vector
    int col_idx = i;
    tripletList.push_back(Triplet(row_idx, col_idx, 1.0));
  }
  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

std::vector<SparseMatrix> get_transpose_coefficients(const Expression& expr) {
  const int rows = size(expr).dims[0];
  const int cols = size(expr).dims[1];

  SparseMatrix coeffs(rows * cols, rows * cols);
  std::vector<Triplet> tripletList;
  tripletList.reserve(rows * cols);
  for (int i = 0; i < rows; i++) {
    for (int j = 0; j < cols; j++) {
      int row_idx = rows * j + i;
      int col_idx = i * cols + j;
      tripletList.push_back(Triplet(row_idx, col_idx, 1.0));
    }
  }
  coeffs.setFromTriplets(tripletList.begin(), tripletList.end());
  coeffs.makeCompressed();
  return {coeffs};
}

typedef std::vector<SparseMatrix>(*CoefficientFunction)(
    const Expression& expr);

std::unordered_map<int, CoefficientFunction> kCoefficientFunctions = {
  {Expression::ADD, &get_add_coefficients},
  {Expression::DIAG_MAT, &get_diag_mat_coefficients},
  {Expression::DIAG_VEC, &get_diag_vec_coefficients},
  {Expression::HSTACK, &get_hstack_coefficients},
  {Expression::INDEX, &get_index_coefficients},
  {Expression::NEG, &get_neg_coefficients},
  {Expression::RESHAPE, &get_reshape_coefficients},
  {Expression::SUM_ENTRIES, &get_sum_entries_coefficients},
  {Expression::TRANSPOSE, &get_transpose_coefficients},
  {Expression::VSTACK, &get_vstack_coefficients},
};

// result += lhs*rhs
void multiply_by_constant(
    const SparseMatrix& lhs, const CoeffMap& rhs, CoeffMap* result) {
  for (const auto& iter : rhs) {
    VLOG(3) << "multiplying\n"
            << "lhs:\n" << matrix_debug_string(lhs)
            << "rhs:\n" << matrix_debug_string(iter.second);

    CHECK_EQ(lhs.cols(), iter.second.rows());
    SparseMatrix value = lhs*iter.second;
    auto ret = result->insert(std::make_pair(iter.first, value));
    if (!ret.second)
      ret.first->second += value;
  }
}

CoeffMap get_coefficients(const Expression& expr) {
  VLOG(2) << "get_coefficients\n" << tree_format_expression(expr);

  CoeffMap coeffs;

  if (expr.type() == Expression::CONST) {
    coeffs[kConstCoefficientId] = to_vector(
        expr.attr<ConstAttributes>().dense_data).sparseView();
  } else if (expr.type() == Expression::VAR) {
    coeffs[expr.attr<VarAttributes>().id] = identity(dim(expr));
  } else if (expr.type() == Expression::MUL) {
    // Special case for binary mul operator which is guaranteed to have one
    // constant argument by DCP rules.
    assert(expr.args().size() == 2);
    CoeffMap lhs_coeffs = get_coefficients(expr.arg(0));
    CoeffMap rhs_coeffs = get_coefficients(expr.arg(1));

    if (is_constant(lhs_coeffs)) {
      std::vector<SparseMatrix> f_coeffs = get_left_mul_coefficients(
          expr, lhs_coeffs[kConstCoefficientId]);
      multiply_by_constant(f_coeffs[0], rhs_coeffs, &coeffs);
    } else if (is_constant(rhs_coeffs)) {
      std::vector<SparseMatrix> f_coeffs = get_right_mul_coefficients(
          expr, rhs_coeffs[kConstCoefficientId]);
      multiply_by_constant(f_coeffs[0], lhs_coeffs, &coeffs);
    } else {
      LOG(FATAL) << "multipying two non constants";
    }
  } else {
    auto iter = kCoefficientFunctions.find(expr.type());
    CHECK(iter != kCoefficientFunctions.end())
        << "no linear coefficients for " << format_expression(expr);
    std::vector<SparseMatrix> f_coeffs = iter->second(expr);
    for (int i = 0; i < expr.args().size(); i++) {
      std::map<int, SparseMatrix> arg_coeffs = get_coefficients(expr.arg(i));
      VLOG(2) << "multiply_by_constant " << format_expression(expr) << " " << i;
      multiply_by_constant(f_coeffs[i], arg_coeffs, &coeffs);
    }
  }

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "get_coefficients done\n" << tree_format_expression(expr);
    for (const auto& iter : coeffs) {
      VLOG(2) <<  "id: " << iter.first << "\n"
              << matrix_debug_string(iter.second);
    }
  }

  return coeffs;
}
