/* GTK - The GIMP Toolkit
 *   
 * Copyright © 2017 Benjamin Otte <otte@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gskslexpressionprivate.h"

#include "gskslpreprocessorprivate.h"
#include "gskslfunctionprivate.h"
#include "gskslnodeprivate.h"
#include "gskslpointertypeprivate.h"
#include "gskslscopeprivate.h"
#include "gsksltokenizerprivate.h"
#include "gsksltypeprivate.h"
#include "gskslvariableprivate.h"
#include "gskspvwriterprivate.h"

#include <string.h>

typedef struct _GskSlExpressionClass GskSlExpressionClass;

struct _GskSlExpression {
  const GskSlExpressionClass *class;
  guint ref_count;
};

struct _GskSlExpressionClass {
  void                  (* free)                                (GskSlExpression        *expression);

  void                  (* print)                               (const GskSlExpression  *expression,
                                                                 GString                *string);
  GskSlType *           (* get_return_type)                     (const GskSlExpression  *expression);
  gboolean              (* is_constant)                         (const GskSlExpression  *expression);
  guint32               (* write_spv)                           (const GskSlExpression  *expression,
                                                                 GskSpvWriter           *writer);
};

static GskSlExpression *
gsk_sl_expression_alloc (const GskSlExpressionClass *klass,
                         gsize                 size)
{
  GskSlExpression *expression;

  expression = g_slice_alloc0 (size);

  expression->class = klass;
  expression->ref_count = 1;

  return expression;
}
#define gsk_sl_expression_new(_name, _klass) ((_name *) gsk_sl_expression_alloc ((_klass), sizeof (_name)))

/* ASSIGNMENT */

typedef struct _GskSlExpressionAssignment GskSlExpressionAssignment;

struct _GskSlExpressionAssignment {
  GskSlExpression parent;

  GskSlTokenType op;
  GskSlExpression *lvalue;
  GskSlExpression *rvalue;
};

static void
gsk_sl_expression_assignment_free (GskSlExpression *expression)
{
  GskSlExpressionAssignment *assignment = (GskSlExpressionAssignment *) expression;

  gsk_sl_expression_unref (assignment->lvalue);
  if (assignment->rvalue)
    gsk_sl_expression_unref (assignment->rvalue);

  g_slice_free (GskSlExpressionAssignment, assignment);
}

static void
gsk_sl_expression_assignment_print (const GskSlExpression *expression,
                                    GString               *string)
{
  const GskSlExpressionAssignment *assignment = (const GskSlExpressionAssignment *) expression;

  gsk_sl_expression_print (assignment->lvalue, string);

  switch ((guint) assignment->op)
  {
    case GSK_SL_TOKEN_EQUAL:
      g_string_append (string, " = ");
      break;
    case GSK_SL_TOKEN_MUL_ASSIGN:
      g_string_append (string, " *= ");
      break;
    case GSK_SL_TOKEN_DIV_ASSIGN:
      g_string_append (string, " /= ");
      break;
    case GSK_SL_TOKEN_MOD_ASSIGN:
      g_string_append (string, " %= ");
      break;
    case GSK_SL_TOKEN_ADD_ASSIGN:
      g_string_append (string, " += ");
      break;
    case GSK_SL_TOKEN_SUB_ASSIGN:
      g_string_append (string, " -= ");
      break;
    case GSK_SL_TOKEN_LEFT_ASSIGN:
      g_string_append (string, " <<= ");
      break;
    case GSK_SL_TOKEN_RIGHT_ASSIGN:
      g_string_append (string, " >>= ");
      break;
    case GSK_SL_TOKEN_AND_ASSIGN:
      g_string_append (string, " &= ");
      break;
    case GSK_SL_TOKEN_XOR_ASSIGN:
      g_string_append (string, " ^= ");
      break;
    case GSK_SL_TOKEN_OR_ASSIGN:
      g_string_append (string, " |= ");
      break;
    default:
      g_assert_not_reached ();
      break;
  }
  gsk_sl_expression_print (assignment->rvalue, string);
}

static GskSlType *
gsk_sl_expression_assignment_get_return_type (const GskSlExpression *expression)
{
  const GskSlExpressionAssignment *assignment = (const GskSlExpressionAssignment *) expression;

  return gsk_sl_expression_get_return_type (assignment->lvalue);
}

static gboolean
gsk_sl_expression_assignment_is_constant (const GskSlExpression *expression)
{
  return FALSE;
}

static guint32
gsk_sl_expression_assignment_write_spv (const GskSlExpression *expression,
                                        GskSpvWriter    *writer)
{
  g_assert_not_reached ();

  return 0;
}

static const GskSlExpressionClass GSK_SL_EXPRESSION_ASSIGNMENT = {
  gsk_sl_expression_assignment_free,
  gsk_sl_expression_assignment_print,
  gsk_sl_expression_assignment_get_return_type,
  gsk_sl_expression_assignment_is_constant,
  gsk_sl_expression_assignment_write_spv
};

/* BINARY */

typedef enum {
  GSK_SL_OPERATION_MUL,
  GSK_SL_OPERATION_DIV,
  GSK_SL_OPERATION_MOD,
  GSK_SL_OPERATION_ADD,
  GSK_SL_OPERATION_SUB,
  GSK_SL_OPERATION_LSHIFT,
  GSK_SL_OPERATION_RSHIFT,
  GSK_SL_OPERATION_LESS,
  GSK_SL_OPERATION_GREATER,
  GSK_SL_OPERATION_LESS_EQUAL,
  GSK_SL_OPERATION_GREATER_EQUAL,
  GSK_SL_OPERATION_EQUAL,
  GSK_SL_OPERATION_NOT_EQUAL,
  GSK_SL_OPERATION_AND,
  GSK_SL_OPERATION_XOR,
  GSK_SL_OPERATION_OR,
  GSK_SL_OPERATION_LOGICAL_AND,
  GSK_SL_OPERATION_LOGICAL_XOR,
  GSK_SL_OPERATION_LOGICAL_OR
} GskSlOperation;

typedef struct _GskSlExpressionOperation GskSlExpressionOperation;

struct _GskSlExpressionOperation {
  GskSlExpression parent;

  GskSlOperation op;
  GskSlExpression *left;
  GskSlExpression *right;
};

static void
gsk_sl_expression_operation_free (GskSlExpression *expression)
{
  GskSlExpressionOperation *operation = (GskSlExpressionOperation *) expression;

  gsk_sl_expression_unref (operation->left);
  if (operation->right)
    gsk_sl_expression_unref (operation->right);

  g_slice_free (GskSlExpressionOperation, operation);
}

static void
gsk_sl_expression_operation_print (const GskSlExpression *expression,
                                   GString               *string)
{
  const char *op_str[] = {
    [GSK_SL_OPERATION_MUL] = " * ",
    [GSK_SL_OPERATION_DIV] = " / ",
    [GSK_SL_OPERATION_MOD] = " % ",
    [GSK_SL_OPERATION_ADD] = " + ",
    [GSK_SL_OPERATION_SUB] = " - ",
    [GSK_SL_OPERATION_LSHIFT] = " << ",
    [GSK_SL_OPERATION_RSHIFT] = " >> ",
    [GSK_SL_OPERATION_LESS] = " < ",
    [GSK_SL_OPERATION_GREATER] = " > ",
    [GSK_SL_OPERATION_LESS_EQUAL] = " <= ",
    [GSK_SL_OPERATION_GREATER_EQUAL] = " >= ",
    [GSK_SL_OPERATION_EQUAL] = " == ",
    [GSK_SL_OPERATION_NOT_EQUAL] = " != ",
    [GSK_SL_OPERATION_AND] = " & ",
    [GSK_SL_OPERATION_XOR] = " ^ ",
    [GSK_SL_OPERATION_OR] = " | ",
    [GSK_SL_OPERATION_LOGICAL_AND] = " && ",
    [GSK_SL_OPERATION_LOGICAL_XOR] = " ^^ ",
    [GSK_SL_OPERATION_LOGICAL_OR] = " || "
  };
  GskSlExpressionOperation *operation = (GskSlExpressionOperation *) expression;

  /* XXX: figure out the need for bracketing here */

  gsk_sl_expression_print (operation->left, string);
  g_string_append (string, op_str[operation->op]);
  gsk_sl_expression_print (operation->right, string);
}

static GskSlType *
gsk_sl_expression_arithmetic_type_check (GskSlPreprocessor *stream,
                                         gboolean           multiply,
                                         GskSlType         *ltype,
                                         GskSlType         *rtype)
{
  GskSlScalarType scalar;

  if (gsk_sl_scalar_type_can_convert (gsk_sl_type_get_scalar_type (ltype),
                                      gsk_sl_type_get_scalar_type (rtype)))
    scalar = gsk_sl_type_get_scalar_type (ltype);
  else if (gsk_sl_scalar_type_can_convert (gsk_sl_type_get_scalar_type (rtype),
                                           gsk_sl_type_get_scalar_type (ltype)))
    scalar = gsk_sl_type_get_scalar_type (rtype);
  else
    {
      if (stream)
        {
          gsk_sl_preprocessor_error (stream, "Operand types %s and %s do not share compatible scalar types.",
                                             gsk_sl_type_get_name (ltype), gsk_sl_type_get_name (rtype));
        }
      return NULL;
    }

  if (gsk_sl_type_is_matrix (ltype))
    {
      if (gsk_sl_type_is_matrix (rtype))
        {
          if (multiply)
            {
              if (gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (gsk_sl_type_get_index_type (rtype)))
                {
                  if (stream)
                    gsk_sl_preprocessor_error (stream, "Matrices to multiplication have incompatible dimensions.");
                  return NULL;
                }
              return gsk_sl_type_get_matrix (scalar,
                                             gsk_sl_type_get_length (gsk_sl_type_get_index_type (ltype)),
                                             gsk_sl_type_get_length (rtype));
            }
          else
            {
              if (gsk_sl_type_can_convert (ltype, rtype))
                {
                  return ltype;
                }
              else if (gsk_sl_type_can_convert (rtype, ltype))
                {
                  return rtype;
                }
              else
                {
                  if (stream)
                    gsk_sl_preprocessor_error (stream, "Matrix types %s and %s have different size.",
                                               gsk_sl_type_get_name (ltype), gsk_sl_type_get_name (rtype));
                  return NULL;
                }
            }
        }
      else if (gsk_sl_type_is_vector (rtype))
        {
          if (multiply)
            {
              if (gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (rtype))
                {
                  if (stream)
                    gsk_sl_preprocessor_error (stream, "Matrix column count doesn't match vector length.");
                  return NULL;
                }
              return gsk_sl_type_get_vector (scalar, gsk_sl_type_get_length (gsk_sl_type_get_index_type (ltype)));
            }
          else
            {
              if (stream)
                gsk_sl_preprocessor_error (stream, "Cannot perform arithmetic operation between matrix and vector.");
              return NULL;
            }
        }
      else if (gsk_sl_type_is_scalar (rtype))
        {
          return gsk_sl_type_get_matrix (scalar,
                                         gsk_sl_type_get_length (ltype),
                                         gsk_sl_type_get_length (gsk_sl_type_get_index_type (ltype)));
        }
      else
        {
          if (stream)
            gsk_sl_preprocessor_error (stream, "Right operand is incompatible type for arithemtic operation.");
          return NULL;
        }
    }
  else if (gsk_sl_type_is_vector (ltype))
    {
      if (gsk_sl_type_is_matrix (rtype))
        {
          if (multiply)
            {
              if (gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (gsk_sl_type_get_index_type (rtype)))
                {
                  if (stream)
                    gsk_sl_preprocessor_error (stream, "Vector length for %s doesn't match row count for %s",
                                                       gsk_sl_type_get_name (ltype), gsk_sl_type_get_name (rtype));
                  return NULL;
                }
              return gsk_sl_type_get_vector (scalar, gsk_sl_type_get_length (rtype));
            }
          else
            {
              if (stream)
                gsk_sl_preprocessor_error (stream, "Cannot perform arithmetic operation between vector and matrix.");
              return NULL;
            }
        }
      else if (gsk_sl_type_is_vector (rtype))
        {
          if (gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (rtype))
            {
              if (stream)
                gsk_sl_preprocessor_error (stream, "Vector operands %s and %s to arithmetic operation have different length.",
                                                   gsk_sl_type_get_name (ltype), gsk_sl_type_get_name (rtype));
              return NULL;
            }
          return gsk_sl_type_get_vector (scalar, gsk_sl_type_get_length (ltype));
        }
      else if (gsk_sl_type_is_scalar (rtype))
        {
          return gsk_sl_type_get_vector (scalar,
                                         gsk_sl_type_get_length (ltype));
        }
      else
        {
          if (stream)
            gsk_sl_preprocessor_error (stream, "Right operand is incompatible type for arithemtic operation.");
          return NULL;
        }
    }
  else if (gsk_sl_type_is_scalar (ltype))
    {
      if (gsk_sl_type_is_matrix (rtype))
        {
          return gsk_sl_type_get_matrix (scalar,
                                         gsk_sl_type_get_length (rtype),
                                         gsk_sl_type_get_length (gsk_sl_type_get_index_type (rtype)));
        }
      else if (gsk_sl_type_is_vector (rtype))
        {
          return gsk_sl_type_get_vector (scalar,
                                         gsk_sl_type_get_length (rtype));
        }
      else if (gsk_sl_type_is_scalar (rtype))
        {
          return gsk_sl_type_get_scalar (scalar);
        }
      else
        {
          if (stream)
            gsk_sl_preprocessor_error (stream, "Right operand is incompatible type for arithemtic operation.");
          return NULL;
        }
    }
  else
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand is incompatible type for arithemtic operation.");
      return NULL;
    }
}

static GskSlType *
gsk_sl_expression_bitwise_type_check (GskSlPreprocessor *stream,
                                      GskSlType         *ltype,
                                      GskSlType         *rtype)
{
  GskSlScalarType lscalar, rscalar;

  lscalar = gsk_sl_type_get_scalar_type (ltype);
  if (lscalar != GSK_SL_INT && lscalar != GSK_SL_UINT)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand %s is not an integer type.", gsk_sl_type_get_name (ltype));
      return NULL;
    }
  rscalar = gsk_sl_type_get_scalar_type (ltype);
  if (rscalar != GSK_SL_INT && rscalar != GSK_SL_UINT)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand %s is not an integer type.", gsk_sl_type_get_name (rtype));
      return NULL;
    }
  if (!gsk_sl_type_is_scalar (ltype) && !gsk_sl_type_is_vector (ltype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand %s is neither a scalar nor a vector.", gsk_sl_type_get_name (ltype));
      return NULL;
    }
  if (!gsk_sl_type_is_scalar (rtype) && !gsk_sl_type_is_vector (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand %s is neither a scalar nor a vector.", gsk_sl_type_get_name (rtype));
      return NULL;
    }
  if (gsk_sl_type_is_vector (ltype) && gsk_sl_type_is_vector (rtype) &&
      gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Vector operands %s and %s do not have the same length.",
                                           gsk_sl_type_get_name (ltype), gsk_sl_type_get_name (rtype));
      return NULL;
    }

  rscalar = lscalar == GSK_SL_UINT ? GSK_SL_UINT : rscalar;
  if (gsk_sl_type_is_scalar (ltype) && gsk_sl_type_is_scalar (rtype))
    return gsk_sl_type_get_scalar (rscalar);
  else
    return gsk_sl_type_get_vector (rscalar, gsk_sl_type_get_length (ltype));
}

static gboolean
gsk_sl_expression_shift_type_check (GskSlPreprocessor *stream,
                                    GskSlType         *ltype,
                                    GskSlType         *rtype)
{
  GskSlScalarType lscalar, rscalar;

  lscalar = gsk_sl_type_get_scalar_type (ltype);
  if (lscalar != GSK_SL_INT && lscalar != GSK_SL_UINT)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand %s is not an integer type.", gsk_sl_type_get_name (ltype));
      return FALSE;
    }
  rscalar = gsk_sl_type_get_scalar_type (ltype);
  if (rscalar != GSK_SL_INT && rscalar != GSK_SL_UINT)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand %s is not an integer type.", gsk_sl_type_get_name (rtype));
      return FALSE;
    }
  if (!gsk_sl_type_is_scalar (ltype) && !gsk_sl_type_is_vector (ltype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand %s is neither a scalar nor a vector.", gsk_sl_type_get_name (ltype));
      return FALSE;
    }
  if (!gsk_sl_type_is_scalar (rtype) && !gsk_sl_type_is_vector (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand %s is neither a scalar nor a vector.", gsk_sl_type_get_name (rtype));
      return FALSE;
    }
  if (gsk_sl_type_is_scalar (ltype) && gsk_sl_type_is_vector (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand to shift cannot be a vector if left operand is a scalar.");
      return FALSE;
    }
  if (gsk_sl_type_is_vector (ltype) && gsk_sl_type_is_vector (rtype) &&
      gsk_sl_type_get_length (ltype) != gsk_sl_type_get_length (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Vector operands do not have the same length.");
      return FALSE;
    }

  return TRUE;
}

static gboolean
gsk_sl_expression_relational_type_check (GskSlPreprocessor *stream,
                                         GskSlType        *ltype,
                                         GskSlType        *rtype)
{
  if (!gsk_sl_type_is_scalar (ltype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand to relational operator is not a scalar.");
      return FALSE;
    }
  if (gsk_sl_type_get_scalar_type (ltype) == GSK_SL_BOOL)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Left operand to relational operator must not be bool.");
      return FALSE;
    }
  if (!gsk_sl_type_is_scalar (rtype))
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand to relational operator is not a scalar.");
      return FALSE;
    }
  if (gsk_sl_type_get_scalar_type (rtype) == GSK_SL_BOOL)
    {
      if (stream)
        gsk_sl_preprocessor_error (stream, "Right operand to relational operator must not be bool.");
      return FALSE;
    }

  return TRUE;
}

static GskSlType *
gsk_sl_expression_operation_get_return_type (const GskSlExpression *expression)
{
  GskSlExpressionOperation *operation = (GskSlExpressionOperation *) expression;

  switch (operation->op)
  {
    case GSK_SL_OPERATION_MUL:
      return gsk_sl_expression_arithmetic_type_check (NULL,
                                                      TRUE,
                                                      gsk_sl_expression_get_return_type (operation->left),
                                                      gsk_sl_expression_get_return_type (operation->right));
    case GSK_SL_OPERATION_DIV:
    case GSK_SL_OPERATION_ADD:
    case GSK_SL_OPERATION_SUB:
      return gsk_sl_expression_arithmetic_type_check (NULL,
                                                      FALSE,
                                                      gsk_sl_expression_get_return_type (operation->left),
                                                      gsk_sl_expression_get_return_type (operation->right));
    case GSK_SL_OPERATION_LSHIFT:
    case GSK_SL_OPERATION_RSHIFT:
      return gsk_sl_expression_get_return_type (operation->left);
    case GSK_SL_OPERATION_MOD:
    case GSK_SL_OPERATION_AND:
    case GSK_SL_OPERATION_XOR:
    case GSK_SL_OPERATION_OR:
      return gsk_sl_expression_bitwise_type_check (NULL,
                                                   gsk_sl_expression_get_return_type (operation->left),
                                                   gsk_sl_expression_get_return_type (operation->right));
    case GSK_SL_OPERATION_LESS:
    case GSK_SL_OPERATION_GREATER:
    case GSK_SL_OPERATION_LESS_EQUAL:
    case GSK_SL_OPERATION_GREATER_EQUAL:
    case GSK_SL_OPERATION_EQUAL:
    case GSK_SL_OPERATION_NOT_EQUAL:
    case GSK_SL_OPERATION_LOGICAL_AND:
    case GSK_SL_OPERATION_LOGICAL_XOR:
    case GSK_SL_OPERATION_LOGICAL_OR:
      return gsk_sl_type_get_scalar (GSK_SL_BOOL);
    default:
      g_assert_not_reached ();
      return NULL;
  }
}

static gboolean
gsk_sl_expression_operation_is_constant (const GskSlExpression *expression)
{
  const GskSlExpressionOperation *operation = (const GskSlExpressionOperation *) expression;

  return gsk_sl_expression_is_constant (operation->left)
      && gsk_sl_expression_is_constant (operation->right);
}

static guint32
gsk_sl_expression_operation_write_spv (const GskSlExpression *expression,
                                       GskSpvWriter          *writer)
{
  g_assert_not_reached ();

  return 0;
}

static const GskSlExpressionClass GSK_SL_EXPRESSION_OPERATION = {
  gsk_sl_expression_operation_free,
  gsk_sl_expression_operation_print,
  gsk_sl_expression_operation_get_return_type,
  gsk_sl_expression_operation_is_constant,
  gsk_sl_expression_operation_write_spv
};

/* REFERENCE */

typedef struct _GskSlExpressionReference GskSlExpressionReference;

struct _GskSlExpressionReference {
  GskSlExpression parent;

  GskSlVariable *variable;
};

static void
gsk_sl_expression_reference_free (GskSlExpression *expression)
{
  GskSlExpressionReference *reference = (GskSlExpressionReference *) expression;

  gsk_sl_variable_unref (reference->variable);

  g_slice_free (GskSlExpressionReference, reference);
}

static void
gsk_sl_expression_reference_print (const GskSlExpression *expression,
                                   GString               *string)
{
  const GskSlExpressionReference *reference = (const GskSlExpressionReference *) expression;

  g_string_append (string, gsk_sl_variable_get_name (reference->variable));
}

static GskSlType *
gsk_sl_expression_reference_get_return_type (const GskSlExpression *expression)
{
  const GskSlExpressionReference *reference = (const GskSlExpressionReference *) expression;

  return gsk_sl_pointer_type_get_type (gsk_sl_variable_get_type (reference->variable));
}

static gboolean
gsk_sl_expression_reference_is_constant (const GskSlExpression *expression)
{
  return FALSE;
}

static guint32
gsk_sl_expression_reference_write_spv (const GskSlExpression *expression,
                                       GskSpvWriter          *writer)
{
  GskSlExpressionReference *reference = (GskSlExpressionReference *) expression;
  guint32 declaration_id, result_id, type_id;

  type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_pointer_type_get_type (gsk_sl_variable_get_type (reference->variable)));
  declaration_id = gsk_spv_writer_get_id_for_variable (writer, reference->variable);
  result_id = gsk_spv_writer_next_id (writer);
  gsk_spv_writer_add (writer,
                      GSK_SPV_WRITER_SECTION_CODE,
                      4, GSK_SPV_OP_LOAD,
                      (guint32[3]) { type_id,
                                     result_id,
                                     declaration_id });

  return result_id;
}

static const GskSlExpressionClass GSK_SL_EXPRESSION_REFERENCE = {
  gsk_sl_expression_reference_free,
  gsk_sl_expression_reference_print,
  gsk_sl_expression_reference_get_return_type,
  gsk_sl_expression_reference_is_constant,
  gsk_sl_expression_reference_write_spv
};

/* FUNCTION_CALL */

typedef struct _GskSlExpressionFunctionCall GskSlExpressionFunctionCall;

struct _GskSlExpressionFunctionCall {
  GskSlExpression parent;

  GskSlFunction *function;
  GskSlExpression **arguments;
  guint n_arguments;
};

static void
gsk_sl_expression_function_call_free (GskSlExpression *expression)
{
  GskSlExpressionFunctionCall *function_call = (GskSlExpressionFunctionCall *) expression;
  guint i;

  for (i = 0; i < function_call->n_arguments; i++)
    {
      gsk_sl_expression_unref (function_call->arguments[i]);
    }
  g_free (function_call->arguments);

  gsk_sl_function_unref (function_call->function);

  g_slice_free (GskSlExpressionFunctionCall, function_call);
}

static void
gsk_sl_expression_function_call_print (const GskSlExpression *expression,
                                       GString               *string)
{
  const GskSlExpressionFunctionCall *function_call = (const GskSlExpressionFunctionCall *) expression;
  guint i;

  gsk_sl_function_print_name (function_call->function, string);
  g_string_append (string, " (");
  
  for (i = 0; i < function_call->n_arguments; i++)
    {
      if (i > 0)
        g_string_append (string, ", ");
      gsk_sl_expression_print (function_call->arguments[i], string);
    }

  g_string_append (string, ")");
}

static GskSlType *
gsk_sl_expression_function_call_get_return_type (const GskSlExpression *expression)
{
  const GskSlExpressionFunctionCall *function_call = (const GskSlExpressionFunctionCall *) expression;

  return gsk_sl_function_get_return_type (function_call->function);
}

static gboolean
gsk_sl_expression_function_call_is_constant (const GskSlExpression *expression)
{
  return FALSE;
}

static guint32
gsk_sl_expression_function_call_write_spv (const GskSlExpression *expression,
                                           GskSpvWriter          *writer)
{
  g_assert_not_reached ();

  return 0;
}

static const GskSlExpressionClass GSK_SL_EXPRESSION_FUNCTION_CALL = {
  gsk_sl_expression_function_call_free,
  gsk_sl_expression_function_call_print,
  gsk_sl_expression_function_call_get_return_type,
  gsk_sl_expression_function_call_is_constant,
  gsk_sl_expression_function_call_write_spv
};

/* CONSTANT */

typedef struct _GskSlExpressionConstant GskSlExpressionConstant;

struct _GskSlExpressionConstant {
  GskSlExpression parent;

  GskSlScalarType type;
  union {
    gint32       i32;
    guint32      u32;
    float        f;
    double       d;
    gboolean     b;
  };
};

static void
gsk_sl_expression_constant_free (GskSlExpression *expression)
{
  GskSlExpressionConstant *constant = (GskSlExpressionConstant *) expression;

  g_slice_free (GskSlExpressionConstant, constant);
}

static void
gsk_sl_expression_constant_print (const GskSlExpression *expression,
                                  GString               *string)
{
  const GskSlExpressionConstant *constant = (const GskSlExpressionConstant *) expression;
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  switch (constant->type)
  {
    case GSK_SL_FLOAT:
      g_ascii_dtostr (buf, G_ASCII_DTOSTR_BUF_SIZE, constant->f);
      g_string_append (string, buf);
      if (strchr (buf, '.') == NULL)
        g_string_append (string, ".0");
      break;

    case GSK_SL_DOUBLE:
      g_ascii_dtostr (buf, G_ASCII_DTOSTR_BUF_SIZE, constant->d);
      g_string_append (string, buf);
      if (strchr (buf, '.') == NULL)
        g_string_append (string, ".0");
      g_string_append (string, "lf");
      break;

    case GSK_SL_INT:
      g_string_append_printf (string, "%i", (gint) constant->i32);
      break;

    case GSK_SL_UINT:
      g_string_append_printf (string, "%uu", (guint) constant->u32);
      break;

    case GSK_SL_BOOL:
      g_string_append (string, constant->b ? "true" : "false");
      break;

    case GSK_SL_VOID:
    default:
      g_assert_not_reached ();
      break;
  }
}

static GskSlType *
gsk_sl_expression_constant_get_return_type (const GskSlExpression *expression)
{
  const GskSlExpressionConstant *constant = (const GskSlExpressionConstant *) expression;

  return gsk_sl_type_get_scalar (constant->type);
}

static gboolean
gsk_sl_expression_constant_is_constant (const GskSlExpression *expression)
{
  return TRUE;
}

static guint32
gsk_sl_expression_constant_write_spv (const GskSlExpression *expression,
                                      GskSpvWriter          *writer)
{
  const GskSlExpressionConstant *constant = (const GskSlExpressionConstant *) expression;
  guint32 type_id, result_id;

  switch (constant->type)
  {
    case GSK_SL_FLOAT:
      type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_type_get_scalar (GSK_SL_FLOAT));
      result_id = gsk_spv_writer_next_id (writer);
      gsk_spv_writer_add (writer,
                          GSK_SPV_WRITER_SECTION_DECLARE,
                          4, GSK_SPV_OP_CONSTANT,
                          (guint32[3]) { type_id,
                                         result_id,
                                         *(guint32 *) &constant->f });
      break;

    case GSK_SL_DOUBLE:
      type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_type_get_scalar (GSK_SL_DOUBLE));
      result_id = gsk_spv_writer_next_id (writer);
      gsk_spv_writer_add (writer,
                          GSK_SPV_WRITER_SECTION_DECLARE,
                          5, GSK_SPV_OP_CONSTANT,
                          (guint32[4]) { type_id,
                                         result_id,
                                         *(guint32 *) &constant->d,
                                         *(((guint32 *) &constant->d) + 1) });
      break;

    case GSK_SL_INT:
      type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_type_get_scalar (GSK_SL_INT));
      result_id = gsk_spv_writer_next_id (writer);
      gsk_spv_writer_add (writer,
                          GSK_SPV_WRITER_SECTION_DECLARE,
                          4, GSK_SPV_OP_CONSTANT,
                          (guint32[3]) { type_id,
                                         result_id,
                                         constant->i32 });
      break;

    case GSK_SL_UINT:
      type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_type_get_scalar (GSK_SL_UINT));
      result_id = gsk_spv_writer_next_id (writer);
      gsk_spv_writer_add (writer,
                          GSK_SPV_WRITER_SECTION_DECLARE,
                          4, GSK_SPV_OP_CONSTANT,
                          (guint32[3]) { type_id,
                                         result_id,
                                         constant->u32 });
      break;

    case GSK_SL_BOOL:
      type_id = gsk_spv_writer_get_id_for_type (writer, gsk_sl_type_get_scalar (GSK_SL_BOOL));
      result_id = gsk_spv_writer_next_id (writer);
      gsk_spv_writer_add (writer,
                          GSK_SPV_WRITER_SECTION_DECLARE,
                          3, constant->b ? GSK_SPV_OP_CONSTANT_TRUE : GSK_SPV_OP_CONSTANT_FALSE,
                          (guint32[2]) { type_id,
                                         result_id });
      break;

    case GSK_SL_VOID:
    default:
      g_assert_not_reached ();
      break;
  }

  return result_id;
}

static const GskSlExpressionClass GSK_SL_EXPRESSION_CONSTANT = {
  gsk_sl_expression_constant_free,
  gsk_sl_expression_constant_print,
  gsk_sl_expression_constant_get_return_type,
  gsk_sl_expression_constant_is_constant,
  gsk_sl_expression_constant_write_spv
};

GskSlExpression *
gsk_sl_expression_parse_constructor_call (GskSlScope        *scope,
                                          GskSlPreprocessor *stream,
                                          GskSlType         *type)
{
  GskSlExpressionFunctionCall *call;
  const GskSlToken *token;
  GskSlType **types;
  GError *error = NULL;
  gboolean fail = FALSE;
  guint i;

  call = gsk_sl_expression_new (GskSlExpressionFunctionCall, &GSK_SL_EXPRESSION_FUNCTION_CALL);
  call->function = gsk_sl_function_new_constructor (type);
  g_assert (call->function);

  token = gsk_sl_preprocessor_get (stream);
  if (!gsk_sl_token_is (token, GSK_SL_TOKEN_LEFT_PAREN))
    {
      gsk_sl_preprocessor_error (stream, "Expected opening \"(\" when calling constructor");
      gsk_sl_expression_unref ((GskSlExpression *) call);
      return NULL;
    }
  gsk_sl_preprocessor_consume (stream, (GskSlExpression *) call);

  token = gsk_sl_preprocessor_get (stream);
  if (!gsk_sl_token_is (token, GSK_SL_TOKEN_RIGHT_PAREN))
    {
      GPtrArray *arguments;
  
      arguments = g_ptr_array_new ();
      while (TRUE)
        {
          GskSlExpression *expression = gsk_sl_expression_parse_assignment (scope, stream);

          if (expression != NULL)
            g_ptr_array_add (arguments, expression);
          else
            fail = TRUE;
          
          token = gsk_sl_preprocessor_get (stream);
          if (!gsk_sl_token_is (token, GSK_SL_TOKEN_COMMA))
            break;
          gsk_sl_preprocessor_consume (stream, (GskSlExpression *) call);
        }

      call->n_arguments = arguments->len;
      call->arguments = (GskSlExpression **) g_ptr_array_free (arguments, FALSE);
    }

  types = g_newa (GskSlType *, call->n_arguments);
  for (i = 0; i < call->n_arguments; i++)
    types[i] = gsk_sl_expression_get_return_type (call->arguments[i]);
  if (!gsk_sl_function_matches (call->function, types, call->n_arguments, &error))
    {
      gsk_sl_preprocessor_error (stream, "%s", error->message);
      g_clear_error (&error);
      fail = TRUE;
    }

  if (!gsk_sl_token_is (token, GSK_SL_TOKEN_RIGHT_PAREN))
    {
      gsk_sl_preprocessor_error (stream, "Expected closing \")\" after arguments.");
      gsk_sl_expression_unref ((GskSlExpression *) call);
      return NULL;
    }
  gsk_sl_preprocessor_consume (stream, (GskSlExpression *) call);

  if (fail)
    {
      gsk_sl_expression_unref ((GskSlExpression *) call);
      return NULL;
    }

  return (GskSlExpression *) call;
}

static GskSlExpression *
gsk_sl_expression_parse_primary (GskSlScope        *scope,
                                 GskSlPreprocessor *stream)
{
  GskSlExpressionConstant *constant;
  const GskSlToken *token;

  token = gsk_sl_preprocessor_get (stream);
  switch ((guint) token->type)
  {
    case GSK_SL_TOKEN_IDENTIFIER:
      {
        GskSlExpressionReference *reference;
        GskSlVariable *variable;

        variable = gsk_sl_scope_lookup_variable (scope, token->str);
        if (variable == NULL)
          {
            gsk_sl_preprocessor_error (stream, "No variable named \"%s\".", token->str);
            gsk_sl_preprocessor_consume (stream, NULL);
            return NULL;
          }

        reference = gsk_sl_expression_new (GskSlExpressionReference, &GSK_SL_EXPRESSION_REFERENCE);
        reference->variable = gsk_sl_variable_ref (variable);
        gsk_sl_preprocessor_consume (stream, (GskSlExpression *) reference);
        return (GskSlExpression *) reference;
      }

    case GSK_SL_TOKEN_INTCONSTANT:
      constant = gsk_sl_expression_new (GskSlExpressionConstant, &GSK_SL_EXPRESSION_CONSTANT);
      constant->type = GSK_SL_INT;
      constant->i32 = token->i32;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) constant);
      return (GskSlExpression *) constant;

    case GSK_SL_TOKEN_UINTCONSTANT:
      constant = gsk_sl_expression_new (GskSlExpressionConstant, &GSK_SL_EXPRESSION_CONSTANT);
      constant->type = GSK_SL_UINT;
      constant->u32 = token->u32;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) constant);
      return (GskSlExpression *) constant;

    case GSK_SL_TOKEN_FLOATCONSTANT:
      constant = gsk_sl_expression_new (GskSlExpressionConstant, &GSK_SL_EXPRESSION_CONSTANT);
      constant->type = GSK_SL_FLOAT;
      constant->f = token->f;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) constant);
      return (GskSlExpression *) constant;

    case GSK_SL_TOKEN_BOOLCONSTANT:
      constant = gsk_sl_expression_new (GskSlExpressionConstant, &GSK_SL_EXPRESSION_CONSTANT);
      constant->type = GSK_SL_BOOL;
      constant->b = token->b;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) constant);
      return (GskSlExpression *) constant;

    case GSK_SL_TOKEN_DOUBLECONSTANT:
      constant = gsk_sl_expression_new (GskSlExpressionConstant, &GSK_SL_EXPRESSION_CONSTANT);
      constant->type = GSK_SL_DOUBLE;
      constant->d = token->d;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) constant);
      return (GskSlExpression *) constant;

    case GSK_SL_TOKEN_VOID:
    case GSK_SL_TOKEN_FLOAT:
    case GSK_SL_TOKEN_DOUBLE:
    case GSK_SL_TOKEN_INT:
    case GSK_SL_TOKEN_UINT:
    case GSK_SL_TOKEN_BOOL:
    case GSK_SL_TOKEN_BVEC2:
    case GSK_SL_TOKEN_BVEC3:
    case GSK_SL_TOKEN_BVEC4:
    case GSK_SL_TOKEN_IVEC2:
    case GSK_SL_TOKEN_IVEC3:
    case GSK_SL_TOKEN_IVEC4:
    case GSK_SL_TOKEN_UVEC2:
    case GSK_SL_TOKEN_UVEC3:
    case GSK_SL_TOKEN_UVEC4:
    case GSK_SL_TOKEN_VEC2:
    case GSK_SL_TOKEN_VEC3:
    case GSK_SL_TOKEN_VEC4:
    case GSK_SL_TOKEN_DVEC2:
    case GSK_SL_TOKEN_DVEC3:
    case GSK_SL_TOKEN_DVEC4:
    case GSK_SL_TOKEN_MAT2:
    case GSK_SL_TOKEN_MAT3:
    case GSK_SL_TOKEN_MAT4:
    case GSK_SL_TOKEN_DMAT2:
    case GSK_SL_TOKEN_DMAT3:
    case GSK_SL_TOKEN_DMAT4:
    case GSK_SL_TOKEN_MAT2X2:
    case GSK_SL_TOKEN_MAT2X3:
    case GSK_SL_TOKEN_MAT2X4:
    case GSK_SL_TOKEN_MAT3X2:
    case GSK_SL_TOKEN_MAT3X3:
    case GSK_SL_TOKEN_MAT3X4:
    case GSK_SL_TOKEN_MAT4X2:
    case GSK_SL_TOKEN_MAT4X3:
    case GSK_SL_TOKEN_MAT4X4:
    case GSK_SL_TOKEN_DMAT2X2:
    case GSK_SL_TOKEN_DMAT2X3:
    case GSK_SL_TOKEN_DMAT2X4:
    case GSK_SL_TOKEN_DMAT3X2:
    case GSK_SL_TOKEN_DMAT3X3:
    case GSK_SL_TOKEN_DMAT3X4:
    case GSK_SL_TOKEN_DMAT4X2:
    case GSK_SL_TOKEN_DMAT4X3:
    case GSK_SL_TOKEN_DMAT4X4:
      {
        GskSlType *type;

        type = gsk_sl_type_new_parse (stream);
        if (type == NULL)
          return NULL;

        return gsk_sl_expression_parse_constructor_call (scope, stream, type);
      }

    default:
      gsk_sl_preprocessor_error (stream, "Expected an expression.");
      gsk_sl_preprocessor_consume (stream, NULL);
      return NULL;
  }
}

static GskSlExpression *
gsk_sl_expression_parse_postfix (GskSlScope        *scope,
                                 GskSlPreprocessor *stream)
{
  return gsk_sl_expression_parse_primary (scope, stream);
}

static GskSlExpression *
gsk_sl_expression_parse_unary (GskSlScope        *scope,
                               GskSlPreprocessor *stream)
{
  return gsk_sl_expression_parse_postfix (scope, stream);
}

static GskSlExpression *
gsk_sl_expression_parse_multiplicative (GskSlScope        *scope,
                                        GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;
  GskSlOperation op;

  expression = gsk_sl_expression_parse_unary (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (gsk_sl_token_is (token, GSK_SL_TOKEN_STAR))
        op = GSK_SL_OPERATION_MUL;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_SLASH))
        op = GSK_SL_OPERATION_DIV;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_PERCENT))
        op = GSK_SL_OPERATION_MOD;
      else
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = op;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_unary (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if ((op == GSK_SL_OPERATION_MOD &&
                !gsk_sl_expression_bitwise_type_check (stream,
                                                 gsk_sl_expression_get_return_type (operation->left),
                                                 gsk_sl_expression_get_return_type (operation->right))) ||
               (op != GSK_SL_OPERATION_MOD &&
                !gsk_sl_expression_arithmetic_type_check (stream,
                                                    FALSE,
                                                    gsk_sl_expression_get_return_type (operation->left),
                                                    gsk_sl_expression_get_return_type (operation->right))))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_additive (GskSlScope        *scope,
                                  GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;
  GskSlOperation op;

  expression = gsk_sl_expression_parse_multiplicative (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (gsk_sl_token_is (token, GSK_SL_TOKEN_PLUS))
        op = GSK_SL_OPERATION_ADD;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_DASH))
        op = GSK_SL_OPERATION_SUB;
      else
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = op;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_additive (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_arithmetic_type_check (stream,
                                                   FALSE,
                                                   gsk_sl_expression_get_return_type (operation->left),
                                                   gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_shift (GskSlScope        *scope,
                               GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;
  GskSlOperation op;

  expression = gsk_sl_expression_parse_additive (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (gsk_sl_token_is (token, GSK_SL_TOKEN_LEFT_OP))
        op = GSK_SL_OPERATION_LSHIFT;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_RIGHT_OP))
        op = GSK_SL_OPERATION_RSHIFT;
      else
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = op;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_additive (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_shift_type_check (stream,
                                              gsk_sl_expression_get_return_type (operation->left),
                                              gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_relational (GskSlScope        *scope,
                                    GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;
  GskSlOperation op;

  expression = gsk_sl_expression_parse_shift (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (gsk_sl_token_is (token, GSK_SL_TOKEN_LEFT_ANGLE))
        op = GSK_SL_OPERATION_LESS;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_RIGHT_ANGLE))
        op = GSK_SL_OPERATION_GREATER;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_LE_OP))
        op = GSK_SL_OPERATION_LESS_EQUAL;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_GE_OP))
        op = GSK_SL_OPERATION_GREATER_EQUAL;
      else
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = op;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_shift (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_relational_type_check (stream,
                                                   gsk_sl_expression_get_return_type (operation->left),
                                                   gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_equality (GskSlScope        *scope,
                                  GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;
  GskSlOperation op;

  expression = gsk_sl_expression_parse_relational (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (gsk_sl_token_is (token, GSK_SL_TOKEN_EQ_OP))
        op = GSK_SL_OPERATION_EQUAL;
      else if (gsk_sl_token_is (token, GSK_SL_TOKEN_NE_OP))
        op = GSK_SL_OPERATION_NOT_EQUAL;
      else
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = op;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_relational (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_and (GskSlScope        *scope,
                             GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_equality (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_AMPERSAND))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_AND;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_equality (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_bitwise_type_check (stream,
                                                gsk_sl_expression_get_return_type (operation->left),
                                                gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_xor (GskSlScope        *scope,
                             GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_and (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_CARET))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_XOR;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_and (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_bitwise_type_check (stream,
                                                gsk_sl_expression_get_return_type (operation->left),
                                                gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_or (GskSlScope        *scope,
                            GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_xor (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_VERTICAL_BAR))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_OR;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_xor (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_expression_bitwise_type_check (stream,
                                                gsk_sl_expression_get_return_type (operation->left),
                                                gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_logical_and (GskSlScope        *scope,
                                     GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_or (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_AND_OP))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_LOGICAL_AND;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_or (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_preprocessor_error (stream, "Right operand of && expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (operation->right)));
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (expression)))
        {
          gsk_sl_preprocessor_error (stream, "Left operand of && expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (expression)));
          expression = operation->right;
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_logical_xor (GskSlScope        *scope,
                                     GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_logical_and (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_XOR_OP))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_LOGICAL_XOR;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_logical_and (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_preprocessor_error (stream, "Right operand of ^^ expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (operation->right)));
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (expression)))
        {
          gsk_sl_preprocessor_error (stream, "Left operand of ^^ expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (expression)));
          expression = operation->right;
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_logical_or (GskSlScope        *scope,
                                    GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *expression;
  GskSlExpressionOperation *operation;

  expression = gsk_sl_expression_parse_logical_xor (scope, stream);
  if (expression == NULL)
    return NULL;

  while (TRUE)
    {
      token = gsk_sl_preprocessor_get (stream);
      if (!gsk_sl_token_is (token, GSK_SL_TOKEN_OR_OP))
        return expression;

      operation = gsk_sl_expression_new (GskSlExpressionOperation, &GSK_SL_EXPRESSION_OPERATION);
      operation->left = expression;
      operation->op = GSK_SL_OPERATION_LOGICAL_OR;
      gsk_sl_preprocessor_consume (stream, (GskSlExpression *) operation);
      operation->right = gsk_sl_expression_parse_logical_xor (scope, stream);
      if (operation->right == NULL)
        {
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (operation->right)))
        {
          gsk_sl_preprocessor_error (stream, "Right operand of || expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (operation->right)));
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else if (!gsk_sl_type_can_convert (gsk_sl_type_get_scalar (GSK_SL_BOOL),
                                         gsk_sl_expression_get_return_type (expression)))
        {
          gsk_sl_preprocessor_error (stream, "Left operand of || expression is not bool but %s",
                                             gsk_sl_type_get_name (gsk_sl_expression_get_return_type (expression)));
          expression = operation->right;
          gsk_sl_expression_ref (expression);
          gsk_sl_expression_unref ((GskSlExpression *) operation);
        }
      else
        {
          expression = (GskSlExpression *) operation;
        }
    }

  return expression;
}

static GskSlExpression *
gsk_sl_expression_parse_conditional (GskSlScope        *scope,
                                     GskSlPreprocessor *stream)
{
  /* XXX: support conditionals */
  return gsk_sl_expression_parse_logical_or (scope, stream);
}

GskSlExpression *
gsk_sl_expression_parse_assignment (GskSlScope        *scope,
                                    GskSlPreprocessor *stream)
{
  const GskSlToken *token;
  GskSlExpression *lvalue;
  GskSlExpressionAssignment *assign;

  lvalue = gsk_sl_expression_parse_conditional (scope, stream);
  if (lvalue == NULL)
    return NULL;

  token = gsk_sl_preprocessor_get (stream);
  switch ((guint) token->type)
  {
      case GSK_SL_TOKEN_EQUAL:
      case GSK_SL_TOKEN_MUL_ASSIGN:
      case GSK_SL_TOKEN_DIV_ASSIGN:
      case GSK_SL_TOKEN_MOD_ASSIGN:
      case GSK_SL_TOKEN_ADD_ASSIGN:
      case GSK_SL_TOKEN_SUB_ASSIGN:
      case GSK_SL_TOKEN_LEFT_ASSIGN:
      case GSK_SL_TOKEN_RIGHT_ASSIGN:
      case GSK_SL_TOKEN_AND_ASSIGN:
      case GSK_SL_TOKEN_XOR_ASSIGN:
      case GSK_SL_TOKEN_OR_ASSIGN:
        break;

      default:
        return lvalue;
  }

  if (gsk_sl_expression_is_constant (lvalue))
    {
      gsk_sl_preprocessor_error (stream, "Cannot assign to a return lvalue.");

      /* Continue parsing like normal here to get more errors */
      gsk_sl_preprocessor_consume (stream, lvalue);
      gsk_sl_expression_unref (lvalue);

      return gsk_sl_expression_parse_assignment (scope, stream);
    }

  assign = gsk_sl_expression_new (GskSlExpressionAssignment, &GSK_SL_EXPRESSION_ASSIGNMENT);
  assign->lvalue = lvalue;
  assign->op = token->type;

  gsk_sl_preprocessor_consume (stream, (GskSlExpression *) assign);

  assign->rvalue = gsk_sl_expression_parse_assignment (scope, stream);
  if (assign->rvalue == NULL)
    {
      gsk_sl_expression_unref ((GskSlExpression *) assign);
      return lvalue;
    }

  return (GskSlExpression *) assign;
}

GskSlExpression *
gsk_sl_expression_parse (GskSlScope        *scope,
                         GskSlPreprocessor *stream)
{
  /* XXX: Allow comma here */
  return gsk_sl_expression_parse_assignment (scope, stream);
}

GskSlExpression *
gsk_sl_expression_ref (GskSlExpression *expression)
{
  g_return_val_if_fail (expression != NULL, NULL);

  expression->ref_count += 1;

  return expression;
}

void
gsk_sl_expression_unref (GskSlExpression *expression)
{
  if (expression == NULL)
    return;

  expression->ref_count -= 1;
  if (expression->ref_count > 0)
    return;

  expression->class->free (expression);
}

void
gsk_sl_expression_print (const GskSlExpression *expression,
                         GString               *string)
{
  expression->class->print (expression, string);
}

GskSlType *
gsk_sl_expression_get_return_type (const GskSlExpression *expression)
{
  return expression->class->get_return_type (expression);
}

gboolean
gsk_sl_expression_is_constant (const GskSlExpression *expression)
{
  return expression->class->is_constant (expression);
}

guint32
gsk_sl_expression_write_spv (const GskSlExpression *expression,
                             GskSpvWriter          *writer)
{
  return expression->class->write_spv (expression, writer);
}
