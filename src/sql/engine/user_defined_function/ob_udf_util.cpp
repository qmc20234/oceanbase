/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_udf_util.h"
#include "observer/mysql/obsm_utils.h"
#include "lib/file/file_directory_utils.h"
#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/user_defined_function/ob_user_defined_function.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase {
namespace sql {
using namespace common;
using namespace obmysql;

const char* ObUdfUtil::load_function_postfix[5] = {
    "",
    "_init",
    "_deinit",
    "_clear",
    "_add",
};

int ObUdfUtil::calc_udf_result_type(common::ObIAllocator& allocator, const ObUdfFunction* udf_func,
    const share::schema::ObUDFMeta& udf_meta, const common::ObIArray<common::ObString>& udf_attributes,
    const common::ObIArray<ObExprResType>& udf_attributes_types, ObExprResType& type)
{
  int ret = OB_SUCCESS;
  bool init_succ = false;
  // Based on the following facts:
  // 1. OB calculates the result type of expr when it is called ExprDeduceType,
  //    and it is called in the resolver phase/rewrite phase/CG phase. In the CG phase,
  //    the ObAccuracy of ObExprResType will be set to ObPostExprItem as the basis for
  //    data regularization during calculation.
  // 2. The init function of the user UDF can assign values to the UDF_INIT structure,
  //    set max_length and decimal, and specify the precision of the type. It is also
  //    required to execute the init function in the earliest plan generation stage to
  //    obtain max_length and decimal.
  // 3. During the execution of a physical Expr, the UDF_INIT and UDF_ARG structures
  //    used by the udf function interface are used as member variables. Expr needs to
  //    pass these two structures to the user-written function in a non-const manner.
  //    For example, users can perform operations on the memory pointed to by ptr
  //    in the UDF_INIT structure in their own udf function.
  // From the perspective of 2, UDF_INIT and UDF_ARG are static properties; from the perspective of 3,
  // they are more like executing ctx. Under this premise, Expr must use UDF_INIT/UDF_ARG as
  // the ctx component of the execution period, and must execute the init functions
  // to obtain accuracy, making it a necessary parameter in the logic stage. Then a better solution
  // is to first Call the init and deinit functions of udf once, specifically to obtain accuracy.
  // But there is another problem here, that is, if the init function is relatively obsolete or
  // blocked, the generation phase of this plan will be very slow.
  /* do the user defined init func */
  ObUdfFunction::ObUdfCtx udf_ctx;
  if (OB_FAIL(ObUdfUtil::init_udf_args(allocator, udf_attributes, udf_attributes_types, udf_ctx.udf_args_))) {
    LOG_WARN("failed to set udf args", K(ret));
  } else if (OB_FAIL(udf_func->process_init_func(udf_ctx))) {
    LOG_WARN("do agg init func failed", K(ret));
  } else {
    /* further infomation about scale, precision and length, just see the ob_resolver_utils.cpp */
    init_succ = true;
    type.set_default_collation_type();
    switch (udf_meta.ret_) {
      case share::schema::ObUDF::STRING: {
        /* for string and decimal, mysql use char* as result */
        type.set_varchar();
        const ObAccuracy& default_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType];
        /* OB_MAX_VARCHAR_LENGTH(1024*1024) is bigger than ObUdfUtil::MAX_FIELD_WIDTH(255*3 + 1) */
        if (udf_ctx.udf_init_.max_length > ObUdfUtil::UDF_MAX_FIELD_WIDTH) {
          ret = OB_CANT_INITIALIZE_UDF;
          LOG_WARN("the length is invalid", K(ret), K(udf_ctx.udf_init_.max_length));
          LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
        } else if (udf_ctx.udf_init_.max_length != 0) {
          type.set_length((ObLength)udf_ctx.udf_init_.max_length);
        } else {
          type.set_length(default_accuracy.get_length());
          // TODO  how about collation?
          /*
           * type.set_charset_type(charset_type);
           * type.set_collation_type(collation_type);
           * type.set_binary_collation(is_binary);
           * */
        }
        break;
      }
      case share::schema::ObUDF::UDFRetType::DECIMAL: {
        /* for string and decimal, mysql use char* as result */
        type.set_number();
        const ObAccuracy& default_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObNumberType];
        /*
         * set scale
         * OB_MAX_DECIMAL_SCALE is 30, OB_MAX_DOUBLE_FLOAT_PRECISION is 53,
         * ObUdfUtil::DECIMAL_MAX_STR_LENGTH is 9*9 + 2, the same.
         * */
        if (udf_ctx.udf_init_.decimals > OB_MAX_DECIMAL_SCALE) {
          ret = OB_CANT_INITIALIZE_UDF;
          LOG_WARN("the decimal is invalid", K(ret), K(udf_ctx.udf_init_.decimals));
          LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
        } else if (udf_ctx.udf_init_.decimals != 0) {
          type.set_scale((ObScale)udf_ctx.udf_init_.decimals);
        } else {
          type.set_scale(default_accuracy.get_scale());
        }
        /* set precision */
        if (OB_SUCC(ret)) {
          if (udf_ctx.udf_init_.max_length > OB_MAX_DECIMAL_PRECISION) {
            ret = OB_CANT_INITIALIZE_UDF;
            LOG_WARN("the max length is invalid", K(ret), K(udf_ctx.udf_init_.max_length));
            LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
          } else if (udf_ctx.udf_init_.max_length != 0) {
            type.set_precision((ObPrecision)udf_ctx.udf_init_.max_length);
          } else {
            type.set_precision(default_accuracy.get_precision());
          }
        }
        break;
      }
      case share::schema::ObUDF::UDFRetType::REAL: {
        // for real, mysql use double as result
        type.set_double();
        const ObAccuracy& default_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObDoubleType];
        /* set scale */
        if (udf_ctx.udf_init_.decimals > OB_MAX_DOUBLE_FLOAT_SCALE) {
          ret = OB_CANT_INITIALIZE_UDF;
          LOG_WARN("the decimal is invalid", K(ret), K(udf_ctx.udf_init_.decimals));
          LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
        } else if (udf_ctx.udf_init_.decimals != 0) {
          type.set_scale((ObScale)udf_ctx.udf_init_.decimals);
        } else {
          type.set_scale(default_accuracy.get_scale());
        }
        /* set precision */
        if (OB_SUCC(ret)) {
          if (udf_ctx.udf_init_.max_length > OB_MAX_DOUBLE_FLOAT_PRECISION) {
            ret = OB_CANT_INITIALIZE_UDF;
            LOG_WARN("the max length is invalid", K(ret), K(udf_ctx.udf_init_.max_length));
            LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
          } else if (udf_ctx.udf_init_.max_length != 0) {
            type.set_precision((ObPrecision)udf_ctx.udf_init_.max_length);
          } else {
            type.set_precision(default_accuracy.get_precision());
          }
        }
        break;
      }
      case share::schema::ObUDF::UDFRetType::INTEGER: {
        // for integer, mysql use long long as result
        type.set_int();
        const ObAccuracy& default_accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType];
        if (udf_ctx.udf_init_.max_length > OB_MAX_INTEGER_DISPLAY_WIDTH) {
          ret = OB_CANT_INITIALIZE_UDF;
          LOG_WARN("the max length is invalid", K(ret), K(udf_ctx.udf_init_.max_length), K(udf_ctx.udf_init_.decimals));
          LOG_USER_ERROR(OB_CANT_INITIALIZE_UDF, udf_meta.name_.length(), udf_meta.name_.ptr());
        } else if (udf_ctx.udf_init_.max_length != 0) {
          type.set_precision((ObPrecision)udf_ctx.udf_init_.max_length);
          type.set_scale(0);
        } else {
          type.set_precision(default_accuracy.get_precision());
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unhandled udf result type", K(ret));
    }
    /* do udf deinit function */
    if (init_succ) {
      IGNORE_RETURN udf_func->process_deinit_func(udf_ctx);
    }
  }
  LOG_DEBUG("udf get result type", K(type));
  return ret;
}

int ObUdfUtil::load_so(const common::ObString dl, ObUdfSoHandler& handler)
{
  int ret = OB_SUCCESS;
  ObUdfSoHandler handler_tmp = nullptr;
  // Since ObString does not store the last \0, this method is still used, otherwise dlopen will fail.
  int so_name_max_len = OB_MAX_UDF_NAME_LENGTH + ObUdfUtil::UDF_MAX_EXPAND_LENGTH;
  char so_name[OB_MAX_UDF_NAME_LENGTH + ObUdfUtil::UDF_MAX_EXPAND_LENGTH];
  MEMSET(so_name, 0, so_name_max_len);
  MEMCPY(so_name, dl.ptr(), dl.length());
  if (OB_ISNULL(handler_tmp = dlopen(so_name, RTLD_NOW))) {
    /* some error happened */
    ret = OB_CANT_OPEN_LIBRARY;
    char* error_msg = dlerror();
    LOG_WARN("failed to open dl", K(ret), K(error_msg));
  } else {
    /* we got the so handler success */
    handler = handler_tmp;
    LOG_DEBUG("udf get dll handler", K(handler));
  }
  return ret;
}

void ObUdfUtil::unload_so(ObUdfSoHandler& handler)
{
  if (nullptr != handler) {
    dlclose(handler);
  }
}

int ObUdfUtil::deep_copy_udf_args_cell(
    common::ObIAllocator& allocator, int64_t param_num, const common::ObObj* src_objs, common::ObObj*& dst_objs)
{
  int ret = OB_SUCCESS;
  ObObj* objs_tmp = nullptr;
  if (OB_ISNULL(src_objs) || param_num < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid input", K(ret), K(src_objs), K(param_num));
  } else if (param_num == 0) {
  } else if (OB_ISNULL(objs_tmp = (ObObj*)allocator.alloc(param_num * sizeof(ObObj)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < param_num; ++i) {
    if (OB_FAIL(ob_write_obj(allocator, src_objs[i], objs_tmp[i]))) {
      LOG_WARN("deep copy obj failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    dst_objs = objs_tmp;
  }
  return ret;
}

int ObUdfUtil::init_udf_args(ObIAllocator& allocator, const common::ObIArray<common::ObString>& udf_attributes,
    const common::ObIArray<ObExprResType>& udf_attributes_types, ObUdfArgs& udf_args)
{
  int ret = OB_SUCCESS;
  if (udf_attributes.count() > INT64_MAX) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("too much args", K(ret));
  } else if (udf_attributes.count() != udf_attributes_types.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected input", K(ret), K(udf_attributes.count()), K(udf_attributes_types.count()));
  } else {
    udf_args.arg_count = (unsigned int)udf_attributes.count();
    udf_args.args = (char**)allocator.alloc(sizeof(char*) * udf_args.arg_count);
    udf_args.lengths = (unsigned long*)allocator.alloc(sizeof(unsigned long) * udf_args.arg_count);
    udf_args.arg_type = (enum UdfItemResult*)allocator.alloc(sizeof(enum UdfItemResult) * udf_args.arg_count);
    udf_args.maybe_null = (char*)allocator.alloc(sizeof(char) * udf_args.arg_count);
    udf_args.attributes = (char**)allocator.alloc(sizeof(char*) * udf_args.arg_count);
    udf_args.attribute_lengths = (unsigned long*)allocator.alloc(sizeof(unsigned long) * udf_args.arg_count);
  }
  if (OB_SUCC(ret)) {
    if (udf_args.arg_count != 0 && (OB_ISNULL(udf_args.args) || OB_ISNULL(udf_args.lengths) ||
                                       OB_ISNULL(udf_args.arg_type) || OB_ISNULL(udf_args.maybe_null) ||
                                       OB_ISNULL(udf_args.attributes) || OB_ISNULL(udf_args.attribute_lengths))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), K(udf_args.arg_count));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < udf_args.arg_count; ++i) {
    udf_args.lengths[i] = 0;
    udf_args.args[i] = nullptr;
    udf_args.attribute_lengths[i] = udf_attributes.at(i).length();
    if (OB_FAIL(convert_ob_type_to_udf_type(udf_attributes_types.at(i).get_type(), udf_args.arg_type[i]))) {
      LOG_WARN("failt to convert ob type to udf type",
          K(udf_attributes_types.at(i).get_type()),
          K(udf_attributes_types.at(i).get_calc_type()));
    } else if (udf_args.attribute_lengths[i] > 0) {
      if (OB_ISNULL(udf_args.attributes[i] = (char*)allocator.alloc(udf_args.attribute_lengths[i]))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate memory", K(ret), K(udf_args.attribute_lengths[i]));
      } else {
        IGNORE_RETURN MEMCPY(udf_args.attributes[i], udf_attributes.at(i).ptr(), udf_attributes.at(i).length());
      }
    } else {
      udf_args.attributes[i] = nullptr;
    }
    if (OB_SUCC(ret)) {
      udf_args.maybe_null[i] = udf_attributes_types.at(i).has_result_flag(OB_MYSQL_NOT_NULL_FLAG);
    }
  }
  IGNORE_RETURN print_udf_args_to_log(udf_args);
  return ret;
}

int ObUdfUtil::init_const_args(ObIAllocator& allocator, const common::ObIArray<ObUdfConstArgs>& const_results,
    ObUdfArgs& udf_args, ObExprCtx& expr_ctx)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  ObNewRow empty_row;
  const auto& Objs = expr_ctx.phy_plan_ctx_->get_param_store();
  UNUSED(Objs);
  for (int64_t i = 0; i < const_results.count() && OB_SUCC(ret); ++i) {
    const ObUdfConstArgs& const_args = const_results.at(i);
    ObObj value;
    if (const_args.idx_in_udf_arg_ >= udf_args.arg_count || OB_ISNULL(const_args.sql_calc_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid idx", K(ret), K(const_args.idx_in_udf_arg_), K(udf_args.arg_count));
    } else if (OB_FAIL(const_args.sql_calc_->calc(expr_ctx, empty_row, value))) {
      LOG_WARN("failed to calc sql expression", K(ret));
    } else {
      udf_args.args[const_args.idx_in_udf_arg_] = (char*)value.get_data_ptr();
      udf_args.lengths[const_args.idx_in_udf_arg_] = value.get_string_len();
    }
    LOG_DEBUG("calculable value", K(value));
  }
  return ret;
}

int ObUdfUtil::set_udf_args(
    common::ObExprCtx& expr_ctx, int64_t param_num, common::ObObj* src_objs, ObUdfArgs& udf_args)
{
  int ret = OB_SUCCESS;
  // MySQL needs various buffers to copy data to temporary memory for UDF to use.
  // Since we have already deep copyed all the passed ObObj in the previous section,
  // the memory is directly used by the UDF and there is no need to copy it again.
  // The difference is that the int/double area of mysql is continuous. If UDF is used
  // this hidden condition, it may cause core
  if (param_num != udf_args.arg_count) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid input", K(ret), K(param_num), K(udf_args.arg_count));
  }
  for (uint64_t i = 0; i < udf_args.arg_count && OB_SUCC(ret); i++) {
    udf_args.args[i] = 0;
    switch (udf_args.arg_type[i]) {
      case STRING_RESULT:
      case DECIMAL_RESULT: {
        if (src_objs[i].is_null()) {
          udf_args.lengths[i] = 0;
        } else {
          if (!src_objs[i].is_string_type()) {
            EXPR_DEFINE_CAST_CTX(expr_ctx, CM_WARN_ON_FAIL);
            if (OB_FAIL(ObObjCaster::to_type(ObVarcharType, cast_ctx, src_objs[i], src_objs[i]))) {
              LOG_WARN("failed to cast", K(ret));
            }
          }
          if (OB_SUCC(ret)) {
            udf_args.args[i] = (char*)(src_objs[i].get_data_ptr());
            udf_args.lengths[i] = src_objs[i].get_string_len();
          }
        }
        break;
      }
      case INT_RESULT:
        /* int, udf should not use lengths */
        if (OB_UNLIKELY(ObIntTC != src_objs[i].get_type_class())) {
          EXPR_DEFINE_CAST_CTX(expr_ctx, CM_WARN_ON_FAIL);
          if (OB_FAIL(ObObjCaster::to_type(ObIntType, cast_ctx, src_objs[i], src_objs[i]))) {
            LOG_WARN("failed to cast", K(ret));
          }
        }
        if (OB_SUCC(ret) && !src_objs[i].is_null()) {
          int64_t num = src_objs[i].get_int();
          src_objs[i].set_int(num);
          udf_args.args[i] = (char*)src_objs[i].get_data_ptr();
        }
        break;
      case REAL_RESULT:
        /* double, udf should not use lengths */
        if (!src_objs[i].is_double()) {
          EXPR_DEFINE_CAST_CTX(expr_ctx, CM_WARN_ON_FAIL);
          if (OB_FAIL(ObObjCaster::to_type(ObDoubleType, cast_ctx, src_objs[i], src_objs[i]))) {
            LOG_WARN("failed to cast", K(ret));
          }
        }
        if (OB_SUCC(ret) && !src_objs[i].is_null()) {
          udf_args.args[i] = (char*)src_objs[i].get_data_ptr();
        }
        break;
      case ROW_RESULT:
      default:
        // This case should never be chosen
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected type", K(ret), K(udf_args.arg_type[i]));
        break;
    }
  }
  IGNORE_RETURN ObUdfUtil::print_udf_args_to_log(udf_args);
  return ret;
}

int ObUdfUtil::process_udf_func(share::schema::ObUDF::UDFRetType ret_type, common::ObIAllocator& allocator,
    ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAny func_origin_, common::ObObj& result)
{
  int ret = OB_SUCCESS;
  switch (ret_type) {
    case share::schema::ObUDF::STRING: {
      if (OB_FAIL(ObUdfUtil::process_str(allocator, udf_init, udf_args, func_origin_, result))) {
        LOG_WARN("failed to process str", K(ret));
      }
      break;
    }
    case share::schema::ObUDF::DECIMAL: {
      if (OB_FAIL(ObUdfUtil::process_dec(allocator, udf_init, udf_args, func_origin_, result))) {
        LOG_WARN("failed to process dec", K(ret));
      }
      break;
    }
    case share::schema::ObUDF::INTEGER: {
      if (OB_FAIL(ObUdfUtil::process_int(allocator, udf_init, udf_args, func_origin_, result))) {
        LOG_WARN("failed to process int", K(ret));
      }
      break;
    }
    case share::schema::ObUDF::REAL: {
      if (OB_FAIL(ObUdfUtil::process_real(allocator, udf_init, udf_args, func_origin_, result))) {
        LOG_WARN("failed to process real", K(ret));
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected udf ret type", K(ret), K(ret_type));
    }
  }
  return ret;
}

int ObUdfUtil::process_str(
    common::ObIAllocator& allocator, ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAny func, ObObj& result)
{
  int ret = OB_SUCCESS;
  unsigned long res_length = UDF_MAX_FIELD_WIDTH;
  unsigned char is_null_tmp = 0;
  unsigned char error = 0;
  char* str_for_user = (char*)allocator.alloc(UDF_MAX_FIELD_WIDTH);
  IGNORE_RETURN MEMSET(str_for_user, 0, UDF_MAX_FIELD_WIDTH);
  ObUdfFuncString func_str = (ObUdfFuncString)func;
  char* res = func_str(&udf_init, &udf_args, str_for_user, &res_length, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret));
  } else if (nullptr == res || is_null_tmp) {
    /* set null to result */
    result.set_null();
  } else if (res_length >= UDF_MAX_FIELD_WIDTH) {
    /*
     * if res_length is bigger than MAX_FIELD_WIDTH, it must be some error happened.
     * */
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the string is too large, maybe some error happen in user defined function", K(ret), K(res_length));
  } else {
    // result.set_char_value(res, (ObString::obstr_size_t)res_length);
    result.set_string(ObVarcharType, res, (ObString::obstr_size_t)res_length);
    result.set_default_collation_type();
  }
  return ret;
}

int ObUdfUtil::process_dec(
    common::ObIAllocator& allocator, ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAny func, ObObj& result)
{
  int ret = OB_SUCCESS;
  unsigned long res_length = UDF_DECIMAL_MAX_STR_LENGTH;
  unsigned char is_null_tmp = 0;
  unsigned char error = 0;
  char* buf_for_user = (char*)allocator.alloc(UDF_DECIMAL_MAX_STR_LENGTH);
  IGNORE_RETURN MEMSET(buf_for_user, 0, UDF_DECIMAL_MAX_STR_LENGTH);
  ObUdfFuncString func_str = (ObUdfFuncString)func;

  common::number::ObNumber num;
  ObPrecision res_precision = -1;
  ObScale res_scale = -1;

  char* res = func_str(&udf_init, &udf_args, buf_for_user, &res_length, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret));
  } else if (nullptr == res || is_null_tmp) {
    /* set null to result */
    result.set_null();
  } else if (res_length >= UDF_DECIMAL_MAX_STR_LENGTH) {
    /*
     * if res_length is bigger than DECIMAL_MAX_STR_LENGTH, it must be some error happened.
     * */
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the string is too large, maybe some error happen in user defined function", K(ret), K(res_length));
  } else if (OB_FAIL(num.from(res, (long int)res_length, allocator, &res_precision, &res_scale))) {
    LOG_WARN("fail to convert char* to decimal/obnumber", K(ret));
  } else {
    result.set_number(num);
  }
  return ret;
}

int ObUdfUtil::process_int(
    common::ObIAllocator& allocator, ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAny func, ObObj& result)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  unsigned char is_null_tmp = 0;
  unsigned char error = 0;

  ObUdfFuncLonglong func_int = (ObUdfFuncLonglong)func;
  long long int tmp = func_int(&udf_init, &udf_args, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret));
  } else if (is_null_tmp) {
    /* set null to result */
    result.set_null();
  } else {
    result.set_int(tmp);
  }
  return ret;
}

int ObUdfUtil::process_real(
    common::ObIAllocator& allocator, ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAny func, ObObj& result)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  unsigned char is_null_tmp = 0;
  unsigned char error = 0;

  ObUdfFuncDouble func_double = (ObUdfFuncDouble)func;
  double tmp = func_double(&udf_init, &udf_args, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret));
  } else if (is_null_tmp) {
    /* set null to result */
    result.set_null();
  } else {
    result.set_double(tmp);
  }
  return ret;
}

int ObUdfUtil::process_add_func(ObUdfInit& udf_init, ObUdfArgs& udf_args, const ObUdfFuncAdd func)
{
  int ret = OB_SUCCESS;
  unsigned char is_null_tmp = 0;
  unsigned char error = 0;
  IGNORE_RETURN func(&udf_init, &udf_args, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret));
  }
  UNUSED(is_null_tmp);
  return ret;
}

int ObUdfUtil::process_clear_func(ObUdfInit& udf_init, const ObUdfFuncClear func)
{
  int ret = OB_SUCCESS;
  unsigned char error = 0;
  unsigned char is_null_tmp = 0;
  IGNORE_RETURN func(&udf_init, &is_null_tmp, &error);
  if (error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error happened while do the user's code", K(ret), K(error), K(is_null_tmp));
  }
  return ret;
}

void ObUdfUtil::construct_udf_args(ObUdfArgs& args)
{
  args.arg_count = 0;
  args.arg_type = nullptr;
  args.args = nullptr;
  args.attribute_lengths = nullptr;
  args.attributes = nullptr;
  args.extension = nullptr;
  args.maybe_null = nullptr;
}

void ObUdfUtil::construct_udf_init(ObUdfInit& init)
{
  init.const_item = 0;
  init.decimals = 0;
  init.extension = nullptr;
  init.max_length = 0;
  init.maybe_null = 0;
  init.ptr = nullptr;
}

const char* ObUdfUtil::result_type_name(enum UdfItemResult result)
{
  int64_t offset = 0;
  if (INVALID_RESULT == result) {
    offset = 0;
  } else {
    offset = (int64_t)result;
  }
  static const char* result_string[7] = {
      "INVALID_RESULT",
      "STRING_RESULT",
      "REAL_RESULT",
      "INT_RESULT",
      "ROW_RESULT",
      "DECIMAL_RESULT",
  };
  return result_string[offset + 1];
}

void ObUdfUtil::print_udf_args_to_log(const ObUdfArgs& args)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 16> args_strings;
  ObSEArray<ObString, 16> atts_strings;
  ObSEArray<int64_t, 16> maybe_nulls;
  ObSEArray<ObString, 16> arg_type_strings;
  for (int64_t i = 0; i < args.arg_count && OB_SUCC(ret); ++i) {
    if (args.lengths[i] > 0 && OB_FAIL(args_strings.push_back(ObString(args.lengths[i], args.args[i])))) {
      LOG_WARN("push back failed", K(ret));
    }
    if (OB_SUCC(ret)) {
      if (args.attribute_lengths[i] > 0 &&
          OB_FAIL(atts_strings.push_back(ObString(args.attribute_lengths[i], args.attributes[i])))) {
        LOG_WARN("push back failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(maybe_nulls.push_back(args.maybe_null[i] == 1 ? 1 : 0))) {
        LOG_WARN("push back failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(arg_type_strings.push_back(
              ObString(STRLEN(result_type_name(args.arg_type[i])), result_type_name(args.arg_type[i]))))) {
        LOG_WARN("push back failed", K(ret));
      }
    }
  }
  LOG_DEBUG("UDF ARGS",
      K(ret),
      K(args.arg_count),
      K(args_strings),
      K(atts_strings),
      K(maybe_nulls),
      K(arg_type_strings),
      K(lbt()));
}

void ObUdfUtil::print_udf_init_to_log(const ObUdfInit& init)
{
  LOG_DEBUG("UDF INIT", K(init.maybe_null), K(init.decimals), K(init.max_length), K(init.ptr), K(init.const_item));
}

void ObUdfUtil::assign_udf_args(const ObUdfArgs& src_args, ObUdfArgs& dst_args)
{
  dst_args.arg_count = src_args.arg_count;
  dst_args.extension = src_args.extension;
}

void ObUdfUtil::assign_udf_init(const ObUdfInit& src_init, ObUdfInit& dst_init)
{
  dst_init.const_item = src_init.const_item;
  dst_init.decimals = src_init.decimals;
  dst_init.extension = src_init.extension;
  dst_init.max_length = src_init.max_length;
  dst_init.maybe_null = src_init.maybe_null;
  dst_init.ptr = src_init.ptr;
}

int ObUdfUtil::is_udf_ctx_valid(const ObUdfArgs& udf_args, const ObUdfInit& udf_init)
{
  int ret = OB_SUCCESS;
  UNUSED(udf_init);
  if (udf_args.arg_type == nullptr || udf_args.args == nullptr || udf_args.lengths == nullptr ||
      udf_args.maybe_null == nullptr || udf_args.attributes == nullptr || udf_args.attribute_lengths == nullptr) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("some field is null",
        K(udf_args.arg_type),
        K(udf_args.args),
        K(udf_args.maybe_null),
        K(udf_args.attributes),
        K(udf_args.attribute_lengths),
        K(udf_args.arg_count));
  } else {
    for (int64_t i = 0; i < udf_args.arg_count && OB_SUCC(ret); ++i) {
      if (udf_args.args[i] == nullptr || udf_args.attributes[i] == nullptr) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("some field is null", K(ret), K(udf_args.args[i]), K(udf_args.attributes[i]));
      }
    }
  }
  return ret;
}

int ObUdfUtil::convert_ob_type_to_udf_type(common::ObObjType ob_type, UdfItemResult& udf_type)
{
  int ret = OB_SUCCESS;
  // mysql 5.6 type.
  obmysql::EMySQLFieldType mysql_type;
  uint16_t flags;
  ObScale num_decimals;
  if (OB_FAIL(ObSMUtils::get_mysql_type(ob_type, mysql_type, flags, num_decimals))) {
    LOG_WARN("get mysql type failed", K(ret));
  } else if (OB_FAIL(convert_mysql_type_to_udf_type(mysql_type, udf_type))) {
    LOG_WARN("get udf type failed", K(ret));
  }
  LOG_DEBUG("udf type change", K(ob_type), K(mysql_type), K(udf_type));
  return ret;
}

int ObUdfUtil::convert_mysql_type_to_udf_type(const obmysql::EMySQLFieldType& mysql_type, UdfItemResult& udf_type)
{
  int ret = OB_SUCCESS;

  // In the expression item of mysql, it seems that each category returns the hard-coded result type and field type.
  switch (mysql_type) {
    case EMySQLFieldType::MYSQL_TYPE_TINY:
    case EMySQLFieldType::MYSQL_TYPE_SHORT:
    case EMySQLFieldType::MYSQL_TYPE_INT24:
    case EMySQLFieldType::MYSQL_TYPE_LONG:
    case EMySQLFieldType::MYSQL_TYPE_LONGLONG:
      udf_type = INT_RESULT;
      break;
    case EMySQLFieldType::MYSQL_TYPE_DECIMAL:
    case EMySQLFieldType::MYSQL_TYPE_NEWDECIMAL:
    case EMySQLFieldType::MYSQL_TYPE_OB_NUMBER_FLOAT:
      udf_type = DECIMAL_RESULT;
      break;
    case EMySQLFieldType::MYSQL_TYPE_FLOAT:
    case EMySQLFieldType::MYSQL_TYPE_DOUBLE:
      udf_type = REAL_RESULT;
      break;
    case EMySQLFieldType::MYSQL_TYPE_VARCHAR:
    case EMySQLFieldType::MYSQL_TYPE_VAR_STRING:
    case EMySQLFieldType::MYSQL_TYPE_STRING:
    case EMySQLFieldType::MYSQL_TYPE_OB_NCHAR:
    case EMySQLFieldType::MYSQL_TYPE_OB_NVARCHAR2:
    case EMySQLFieldType::MYSQL_TYPE_OB_UROWID:
      udf_type = STRING_RESULT;
      break;
    case EMySQLFieldType::MYSQL_TYPE_YEAR:
      udf_type = INT_RESULT;
      break;
    case EMySQLFieldType::MYSQL_TYPE_TIMESTAMP:
    case EMySQLFieldType::MYSQL_TYPE_DATE:
    case EMySQLFieldType::MYSQL_TYPE_TIME:
    case EMySQLFieldType::MYSQL_TYPE_DATETIME:
    case EMySQLFieldType::MYSQL_TYPE_NEWDATE:
    case EMySQLFieldType::MYSQL_TYPE_BIT:
    // case MYSQL_TYPE_TIMESTAMP2:
    // case MYSQL_TYPE_DATETIME2:
    // case MYSQL_TYPE_TIME2:
    // case MYSQL_TYPE_JSON:
    case EMySQLFieldType::MYSQL_TYPE_ENUM:
    case EMySQLFieldType::MYSQL_TYPE_SET:
    case EMySQLFieldType::MYSQL_TYPE_GEOMETRY:
    case EMySQLFieldType::MYSQL_TYPE_NULL:
    case EMySQLFieldType::MYSQL_TYPE_TINY_BLOB:
    case EMySQLFieldType::MYSQL_TYPE_BLOB:
    case EMySQLFieldType::MYSQL_TYPE_MEDIUM_BLOB:
    case EMySQLFieldType::MYSQL_TYPE_LONG_BLOB:
    case EMySQLFieldType::MYSQL_TYPE_JSON:
      udf_type = STRING_RESULT;
      break;
    default:
      udf_type = INVALID_RESULT;
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unhandled mysql type", K(ret));
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
