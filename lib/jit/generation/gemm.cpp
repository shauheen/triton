/*
 * Copyright (c) 2015, PHILIPPE TILLET. All rights reserved.
 *
 * This file is part of ISAAC.
 *
 * ISAAC is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301  USA
 */

#include "isaac/array.h"
#include "isaac/jit/syntax/expression/preset.h"
#include "isaac/jit/syntax/engine/process.h"
#include "isaac/jit/generation/gemm.h"
#include "isaac/jit/generation/engine/keywords.h"
#include "isaac/exception/api.h"
#include "tools/arguments.hpp"
#include "tools/vector_types.hpp"


#include <string>
#include "isaac/tools/cpp/align.hpp"

namespace isaac
{
namespace templates
{

  uint32_t gemm::lmem_usage(expression_tree const & expression) const
  {
    uint32_t N = 0;
    size_t llda = (A_trans_=='N')?p_.mL:p_.kL+1;
    size_t lnda = (A_trans_=='N')?p_.kL:p_.mL;
    size_t lldb = (B_trans_=='T')?p_.nL:p_.kL+1;
    size_t lndb = (B_trans_=='T')?p_.kL:p_.nL;
    N += llda*lnda;
    N += lldb*lndb;
    return N*size_of(expression.dtype());
  }

  uint32_t gemm::registers_usage(expression_tree const & expression) const
  {
    uint32_t N = p_.mS * p_.nS + p_.mS * p_.kS + p_.kS * p_.nS;
    return N*size_of(expression.dtype());
  }

  uint32_t gemm::temporary_workspace(expression_tree const & expressions) const
  {
      std::vector<int_t> MNK = input_sizes(expressions);
      int_t M = MNK[0]; int_t N = MNK[1];
      if(p_.depth > 1)
        return M*N*p_.depth;
      return 0;
  }

  int gemm::is_invalid_impl(driver::Device const &, expression_tree const &) const
  {
    if(p_.Afetch!=FETCH_FROM_LOCAL || p_.Bfetch!=FETCH_FROM_LOCAL)
      return TEMPLATE_INVALID_FETCHING_POLICY_TYPE;

    if ((p_.mS % p_.vwidth) > 0 || (p_.nS % p_.vwidth) > 0)
      return TEMPLATE_MS_NS_MUST_BE_SIMD_WIDTH_MULTIPLE;

    if(p_.mL > 256 || p_.nL > 256)
       return TEMPLATE_BLOCK_SIZE_TOO_LARGE;

    if ( p_.kS % p_.kL == 0)
      return TEMPLATE_KS_MUST_BE_SMALLER_THAN_KL;

    if (p_.Afetch==FETCH_FROM_LOCAL || p_.Bfetch==FETCH_FROM_LOCAL){
      if ((p_.lf0*p_.lf1) !=(p_.ls0*p_.ls1))
        return TEMPLATE_LOCAL_FETCH_PRODUCT_MUST_MATCH_LOCAL_SIZE_PRODUCT;
    }

    if (p_.Afetch==FETCH_FROM_LOCAL)
    {
      uint32_t bound1 = (A_trans_=='N')?p_.kL:p_.mL;
      uint32_t bound0 = (A_trans_=='N')?p_.mL:p_.kL;

      if (p_.lf1>0 && (bound1 % p_.lf1)> 0)
        return A_trans_=='N'?TEMPLATE_LOCAL_FETCH_1_MUST_BE_KL_MULTIPLE:TEMPLATE_LOCAL_FETCH_1_MUST_BE_ML_MULTIPLE;

      if (p_.lf0>0 && (bound0 % (p_.lf0*p_.vwidth)) > 0)
        return A_trans_=='N'?TEMPLATE_LOCAL_FETCH_0_MUST_BE_NL_MULTIPLE:TEMPLATE_LOCAL_FETCH_0_MUST_BE_KL_MULTIPLE;

    }
    if (p_.Bfetch==FETCH_FROM_LOCAL)
    {
      uint32_t bound1 = (B_trans_=='T')?p_.kL:p_.nL;
      uint32_t bound0 = (B_trans_=='T')?p_.nL:p_.kL;

      if (p_.lf1>0 && (bound1 % p_.lf1)> 0)
        return B_trans_=='T'?TEMPLATE_LOCAL_FETCH_1_MUST_BE_KL_MULTIPLE:TEMPLATE_LOCAL_FETCH_1_MUST_BE_ML_MULTIPLE;

      if (p_.lf0>0 && (bound0 % (p_.lf0*p_.vwidth)) > 0)
        return B_trans_=='T'?TEMPLATE_LOCAL_FETCH_1_MUST_BE_KL_MULTIPLE:TEMPLATE_LOCAL_FETCH_1_MUST_BE_ML_MULTIPLE;

    }

    return TEMPLATE_VALID;
  }

  std::string gemm::generate_impl(std::string const & suffix, expression_tree const & tree, driver::Device const & device, symbolic::symbols_table const &) const
  {
    using std::string;
    using tools::to_string;

    driver::backend_type backend = device.backend();
    bool has_depth = p_.depth > 1;
#define VLOAD(offset, ptr) vload(p_.vwidth, sdtype, offset, ptr, "1", backend, true)
#define VLOAD_MISALIGNED(offset, ptr) vload(p_.vwidth, sdtype, offset, ptr, "1", backend, false)
#define VSTORE(value, offset, ptr) vstore(p_.vwidth, sdtype, value, offset, ptr, "1", backend)

    symbolic::preset::gemm::args args;
    infos(tree, args);
    std::string ASTRIDE1 = (args.A->ld[0] > 1)?"*Astride1":"";
    std::string BSTRIDE1 = (args.B->ld[0] > 1)?"*Bstride1":"";
    std::string CSTRIDE1 = (args.C->ld[0] > 1)?"*Cstride1":"";

    //////////////////
    /// INIT
    /// //////////////
    kernel_generation_stream stream(backend);
    numeric_type dtype = tree.dtype();
    std::string sdtype = to_string(dtype);
    std::string vdtype = append_width(sdtype, p_.vwidth);

    //////////////////
    /// DECLARATIONS
    /// //////////////
    std::string gemm_name = "gemm";
    std::string reduce_name = "reduce";

    gemm_name += suffix;
    reduce_name += suffix;

    switch(backend)
    {
      case driver::OPENCL:
        stream << " __attribute__((reqd_work_group_size(" << p_.ls0 << "," << p_.ls1 << ",1)))" << std::endl;
        break;
      default:
        break;
    }

    stream << "$KERNEL void gemm" << suffix << "($SIZE_T M, $SIZE_T N, $SIZE_T K, "
                               << "$GLOBAL " << sdtype << "* C, $SIZE_T ldc, $SIZE_T offc, $SIZE_T Cstride1, "
                               << sdtype << " alpha,"
                               << "$GLOBAL " << sdtype << "* A, $SIZE_T lda, $SIZE_T offa, $SIZE_T Astride1,"
                               << "$GLOBAL " << sdtype << "* B, $SIZE_T ldb, $SIZE_T offb, $SIZE_T Bstride1,"
                               << sdtype << " beta)"
                               << std::endl;
    stream << "{" << std::endl;
    stream.inc_tab();

    ///Declare
    stream << "//blocks" << std::endl;
    stream << sdtype << " rC[" << p_.mS << "][" << p_.nS << "] = {{0}};" << std::endl;
    stream << vdtype << " rA[" << p_.kS << "][" << p_.mS/p_.vwidth << "];" << std::endl;
    stream << vdtype << " rB[" << p_.kS << "][" << p_.nS/p_.vwidth << "];" << std::endl;
    stream << std::endl;

    stream << "//pointers" << std::endl;
    size_t llda = (A_trans_=='N')?p_.mL:p_.kL+1;
    size_t lnda = (A_trans_=='N')?p_.kL:p_.mL;
    size_t lldb = (B_trans_=='T')?p_.nL:p_.kL+1;
    size_t lndb = (B_trans_=='T')?p_.kL:p_.nL;
    stream << "$LOCAL " << sdtype << " lA[" << llda*lnda << "];" << std::endl;
    stream << "$LOCAL " << sdtype << " lB[" << lldb*lndb << "];" << std::endl;
    uint32_t npA = p_.mL/(A_trans_=='N'?p_.lf0*p_.vwidth:p_.lf1);
    uint32_t npB = p_.nL/(B_trans_=='T'?p_.lf0*p_.vwidth:p_.lf1);
    stream << "$GLOBAL " << sdtype << "* Ai[" << npA << "];" << std::endl;
    stream << "$GLOBAL " << sdtype << "* Bi[" << npB << "];" << std::endl;
    stream << std::endl;

    stream << "//identifiers" << std::endl;
    stream << "int2 idT;" << std::endl;
    stream << "int idt;" << std::endl;
    if(has_depth)
        stream << "int gidz, div, offz;" << std::endl;
    stream << "uint4 ids;" << std::endl;
    stream << "ids.x = $GROUP_IDX_0;" << std::endl;
    stream << "ids.y = $GROUP_IDX_1;" << std::endl;
    stream << "ids.z = $LOCAL_IDX_0;" << std::endl;
    stream << "ids.w = $LOCAL_IDX_1;" << std::endl;
    stream << std::endl;

    stream << "//offsets" << std::endl;
    stream << "A += offa;" << std::endl;
    stream << "B += offb;" << std::endl;
    stream << "C += offc;" << std::endl;

    if(has_depth)
    {
      stream << "gidz = $GROUP_IDX_2;" << std::endl;
      stream << "div = (K+" << p_.depth-1 << ")/" << p_.depth << ";" << std::endl;
      stream << "offz = div*gidz;" << std::endl;
      stream << "K = min(K - div*gidz, ($SIZE_T)div);" << std::endl;
    }

    stream << "idt = " << p_.ls0 << "*ids.w + ids.z;" << std::endl;
    stream << "idT.y = idt/" << p_.lf0 << ";" << std::endl;
    stream << "idT.x = idt - " << p_.lf0 << "*idT.y;" << std::endl;
    stream << std::endl;

    stream << "//Adjust pointers and bounds per work-item" << std::endl;
    stream << "ids.x *= " << p_.mL << ";" << std::endl;
    stream << "ids.y *= " << p_.nL << ";" << std::endl;
    stream << "idT.x *= " << p_.vwidth << ";" << std::endl;

    stream << "M -= ids.x;" << std::endl;
    if(A_trans_=='N')
        stream << "M -= idT.x;" << std::endl;
    else
        stream << "M -= idT.y;" << std::endl;

    stream << "N -= ids.y;" << std::endl;
    if(B_trans_=='T')
        stream << "N -= idT.x;" << std::endl;
    else
        stream << "N -= idT.y;" << std::endl;

    if (A_trans_=='N')
    {
        stream << "A += ids.x" << ASTRIDE1 << ";" << std::endl;
        stream << "A += idT.y*lda;" << std::endl;
        if(has_depth)
            stream << "A += offz*lda;" << std::endl;

    }
    else
    {
        stream << "A += ids.x*lda;" << std::endl;
        stream << "A += idT.x" << ASTRIDE1 << ";" << std::endl;
        if(has_depth)
            stream << "A += offz;" << std::endl;
    }

    if(B_trans_=='T')
    {
        stream << "B += ids.y" << BSTRIDE1 << ";" << std::endl;
        stream << "B += idT.y*ldb;" << std::endl;
        if(has_depth)
            stream << "B += offz*ldb;" << std::endl;
    }
    else
    {
        stream << "B += ids.y*ldb;" << std::endl;
        stream << "B += idT.x" << BSTRIDE1 << ";" << std::endl;
        if(has_depth)
            stream << "B += offz;" << std::endl;
    }

    stream << "#pragma unroll" << std::endl;
    stream << "for(int i = 0 ; i < " << npA << " ; ++i){" << std::endl;
    stream.inc_tab();
    stream << "Ai[i] = A;" << std::endl;
    stream.dec_tab();
    stream << "}" << std::endl;
    stream << std::endl;

    stream << "#pragma unroll" << std::endl;
    stream << "for(int i = 0 ; i < " << npB << " ; ++i){" << std::endl;
    stream.inc_tab();
    stream << "Bi[i] = B;" << std::endl;
    stream.dec_tab();
    stream << "}" << std::endl;
    stream << std::endl;

    for(uint32_t i = 0 ; i < npA ; i++ )
        if (A_trans_=='N')
          stream << "Ai[" << i << "] += " << Select(backend, to_string(i*p_.lf0*p_.vwidth) + " < M", "(int)((idT.x + " + to_string(i*p_.lf0*p_.vwidth) + ")" + ASTRIDE1 + ")", "0") << ";" << std::endl;
        else
          stream << "Ai[" << i << "] += " << Select(backend, to_string(i*p_.lf1) + " < M", "(int)((idT.y + " + to_string(i*p_.lf1) + ")*lda)", "0") << ";" << std::endl;

    for(uint32_t i = 0 ; i < npB ; i++ )
        if (B_trans_=='T')
            stream << "Bi[" << i << "] += " << Select(backend, to_string(i*p_.lf0*p_.vwidth) + " < N", "(int)((idT.x + " + to_string(i*p_.lf0*p_.vwidth) + ")" + BSTRIDE1 + ")", "0") << ";" << std::endl;
        else
            stream << "Bi[" << i << "] += " << Select(backend, to_string(i*p_.lf1) + " < N", "(int)((idT.y + " + to_string(i*p_.lf1) + ")*ldb)", "0") << ";" << std::endl;

    stream << std::endl;
    stream << "//Outer loop" << std::endl;
    stream << "while(K >=" << p_.kL << ")" << std::endl;
    stream << "{" << std::endl;
    stream.inc_tab();


    auto fetch_to_lds = [&](bool last_iteration)
    {
        stream << "$LOCAL_BARRIER;" << std::endl;
        stream << "$LOCAL_PTR " << sdtype << "* ldsA = lA + idT.y*" << llda << " + idT.x;" << std::endl;
        stream << "$LOCAL_PTR " << sdtype << "* ldsB = lB + idT.y*" << lldb << " + idT.x;" << std::endl;

        stream << "//Fetch A to local memory" << std::endl;
        if (A_trans_=='N')
        {
          for(uint32_t k = 0; k < p_.kL; k += p_.lf1)
            for(uint32_t m = 0; m < p_.mL; m += p_.lf0*p_.vwidth)
            {
              std::string mm = to_string(m/(p_.vwidth*p_.lf0));
              std::string kk = to_string(k);
              if(last_iteration)
                  for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                      stream << "ldsA[" << k*llda + m + s << "] = (condy" << k << " && " << s << "< M)? Ai[" << mm << "][" << k << "*lda + " << s << "] : 0;" << std::endl;
              else
                stream << VSTORE(VLOAD_MISALIGNED("0" ,"&Ai[" + mm +"][" + kk + "*lda]"), "0", "ldsA + " + to_string(k*llda+m)) << ";" << std::endl;
            }
        }
        else
        {
            for(uint32_t k = 0; k < p_.kL; k += p_.lf0*p_.vwidth)
            for(uint32_t m = 0; m < p_.mL; m += p_.lf1)
              {
                std::string mm = to_string(m/p_.lf1);
                std::string kk = to_string(k);
                if(last_iteration)
                    for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                        stream << "ldsA[" << m*llda + k + s << "] = condx" << k + s << "? Ai[" << mm << "][" << k + s << ASTRIDE1 << "] : 0;" << std::endl;

                else
                    stream << VSTORE(VLOAD_MISALIGNED("0", "&Ai[" + mm + "][" + kk + ASTRIDE1 + "]"), "0", "ldsA + " + to_string(m*llda+k)) << ";" << std::endl;
              }
        }

        stream << "//Fetch B to local memory" << std::endl;
        if (B_trans_=='T')
        {
          for(uint32_t k = 0; k < p_.kL; k += p_.lf1)
            for(uint32_t n = 0; n < p_.nL; n += p_.lf0*p_.vwidth)
            {
              std::string nn = to_string(n/(p_.vwidth*p_.lf0));
              std::string kk = to_string(k);
              if(last_iteration)
                  for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                      stream << "ldsB[" << k*lldb + n + s << "] = (condy" << k << " && " << s << "< N)? Bi[" <<  nn << "][" << kk << "*ldb +" << s << "] : 0;" << std::endl;
              else
                stream << VSTORE(VLOAD_MISALIGNED("0" ,"&Bi[" + nn +"][" + kk + "*ldb]"), "0", "ldsB + " + to_string(k*lldb+n)) << ";" << std::endl;
            }
        }
        else
        {
          for(uint32_t k = 0; k < p_.kL; k += p_.lf0*p_.vwidth)
            for(uint32_t n = 0; n < p_.nL; n += p_.lf1)
            {
              std::string nn = to_string(n/p_.lf1);
              std::string kk = to_string(k);
              if(last_iteration)
                  for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                      stream << "ldsB[" << n*lldb + k + s << "] = condx" << k + s << "? Bi[" << nn << "][" << k + s << BSTRIDE1 << "] : 0;" << std::endl;

              else
                  stream << VSTORE(VLOAD_MISALIGNED("0", "&Bi[" + nn + "][" + kk + BSTRIDE1 + "]"), "0", "ldsB + " + to_string(n*lldb+k)) << ";" << std::endl;
            }
        }

        if(A_trans_=='N')
            stream << "ldsA = lA + ids.z*" << p_.vwidth << ";" << std::endl;
        else
            stream << "ldsA = lA + ids.z*" << llda*p_.vwidth << ";" << std::endl;

        if(B_trans_=='T')
            stream << "ldsB = lB + ids.w*" << p_.vwidth << ";" << std::endl;
        else
            stream << "ldsB = lB + ids.w*" << lldb*p_.vwidth << ";" << std::endl;

        stream << "$LOCAL_BARRIER;" << std::endl;
        std::string bound = last_iteration?"K":tools::to_string(p_.kL);
        size_t ks = last_iteration?1:p_.kS;
        stream << "//Inner loop" << std::endl;
        stream << "for(uint32_t k = 0; k < " << bound << "; k+=" << ks << "){" << std::endl;
        stream.inc_tab();

        stream << "//Fetch A to registers" << std::endl;
        stream << "#pragma unroll" << std::endl;
        stream << "for(uint32_t kk = 0; kk < " << ks << "; kk++)" << std::endl;
        stream << "#pragma unroll " << p_.mS/p_.vwidth << std::endl;
        stream << "for(uint32_t mm = 0; mm < " << p_.mS/p_.vwidth << "; mm++)" << std::endl;
        stream << "{" << std::endl;
        stream.inc_tab();
        if(A_trans_=='N')
            stream << "rA[kk][mm] = "  << VLOAD("0", "ldsA + k*" + to_string(llda) + " + mm*" + to_string(p_.ls0*p_.vwidth) + "+ kk*" + to_string(llda)) << ";" << std::endl;
        else
        {
            if(p_.vwidth==1)
                stream << "rA[kk][mm] = ldsA[k + mm*" << p_.ls0*llda <<  "+ kk"  << "];" << std::endl;
            else
                for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                    stream << access_vector_type("rA[kk][mm]", s) << " = ldsA[k + (mm*" << p_.vwidth*p_.ls0 << " + " << s << ")*" << llda <<  "+ kk];" << std::endl;
        }

        stream.dec_tab();
        stream << "}" << std::endl;

        stream << "//Fetch B to registers" << std::endl;
        stream << "#pragma unroll " << ks << std::endl;
        stream << "for(uint32_t kk = 0; kk < " << ks << "; kk++)" << std::endl;
        stream << "#pragma unroll " << p_.nS/p_.vwidth << std::endl;
        stream << "for(uint32_t nn = 0; nn < " << p_.nS/p_.vwidth << "; nn++)" << std::endl;
        stream << "{" << std::endl;
        stream.inc_tab();
        if(B_trans_=='T')
            stream << "rB[kk][nn] = " << VLOAD("0", "ldsB + k*" + to_string(lldb) + " + nn*" + to_string(p_.ls1*p_.vwidth)  + "+ kk*" + to_string(lldb)) << ";" << std::endl;
        else
        {
            if(p_.vwidth==1)
                stream << "rB[kk][nn] = ldsB[k"  << " + nn*" << p_.ls1*lldb <<  "+ kk"  << "];" << std::endl;
            else
                for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                    stream << access_vector_type("rB[kk][nn]", s) << " = ldsB[k"  << " + (nn*" << p_.vwidth*p_.ls1 << " + " << s << ")*" << lldb <<  "+ kk];" << std::endl;
        }
        stream.dec_tab();
        stream << "}" << std::endl;

        stream << "//FMA computations" << std::endl;
        stream << "#pragma unroll" << std::endl;
        stream << "for(uint32_t kk = 0 ; kk < " << ks << "; ++kk){" << std::endl;
        stream.inc_tab();
        for(uint32_t nn=0; nn < p_.nS; ++nn)
        for(uint32_t mm=0; mm < p_.mS; ++mm){
          string res_str, lhs_str, rhs_str;
          res_str = "rC[" + to_string(mm) + "][" + to_string(nn) + "]";
          if (p_.vwidth==1)
            lhs_str = "rA[kk][" + to_string(mm) + "]";
          else
            lhs_str = access_vector_type("rA[kk][" + to_string(mm/p_.vwidth) + "]", mm%p_.vwidth);
          if (p_.vwidth==1)
            rhs_str = "rB[kk]["+to_string(nn)+"]";
          else
            rhs_str = access_vector_type("rB[kk]["+to_string(nn/p_.vwidth)+"]", nn%p_.vwidth);
          stream << res_str << "= $MAD(" << lhs_str << "," << rhs_str << "," << res_str << ");" << std::endl;
        }
        stream.dec_tab();
        stream << "}" << std::endl;
        stream.dec_tab();
        stream << "}" << std::endl;
        stream << "K -= " << p_.kL << ";" << std::endl;

        //Increment A pointers to global memory
        if (A_trans_=='N')
          for(uint32_t i = 0 ; i < npA ; ++i)
              stream << "Ai[" << i << "] += "  << p_.kL << "*lda;" << std::endl;
        else
          for(uint32_t i = 0 ; i < npA ; ++i)
              stream << "Ai[" << i << "] += "  << p_.kL << ASTRIDE1 << ";" << std::endl;

        //Increment B pointers to global memory
        if (B_trans_=='T')
          for(uint32_t i = 0 ; i < npB ; ++i)
              stream << "Bi[" << i << "] += " << p_.kL << "*ldb;" << std::endl;
        else
          for(uint32_t i = 0 ; i < npB ; ++i)
              stream << "Bi[" << i << "] += " << p_.kL << BSTRIDE1 << ";" << std::endl;
    };
    fetch_to_lds(false);
    stream.dec_tab();
    stream << "}" << std::endl;


    if(A_trans_=='N' || B_trans_=='T')
    {
        stream << "int Ky = K - idT.y;" << std::endl;
        for(uint32_t k = 0; k < p_.kL; k += p_.lf1)
            stream << "int condy" << k << " = " << k << " < Ky;" << std::endl;
    }

    if(A_trans_=='T' || B_trans_=='N')
    {
        stream << "int Kx = K - idT.x;" << std::endl;
        for(uint32_t k = 0 ; k < p_.kL ; k += p_.lf0*p_.vwidth)
            for(uint32_t s = 0 ; s < p_.vwidth ; ++s)
                stream << "int condx" << k + s << " = " << k + s << " < Kx;" << std::endl;
    }
    fetch_to_lds(true);

    stream << "//Write back C" << std::endl;
    stream << "M += ids.x;" << std::endl;
    if(A_trans_=='N')
        stream << "M += idT.x;" << std::endl;
    else
        stream << "M += idT.y;" << std::endl;

    if(B_trans_=='T')
        stream << "N += idT.x;" << std::endl;
    else
        stream << "N += idT.y;" << std::endl;
    stream << "N += ids.y;" << std::endl;

    stream << "C += ids.x" << CSTRIDE1 << ";" << std::endl;
    stream << "C += ids.z*" << p_.vwidth << CSTRIDE1 << ";" << std::endl;
    stream << "C += ids.y*ldc;" << std::endl;
    stream << "C += ids.w*" << p_.vwidth << "*ldc;" << std::endl;
    if(has_depth)
        stream << "C += gidz*ldc*N;" << std::endl;

    stream << "M -= ids.x;" << std::endl;
    stream << "M -= ids.z*" << p_.vwidth << ";" << std::endl;

    stream << "N -= ids.y;" << std::endl;
    stream << "N -= ids.w*" << p_.vwidth <<  ";" << std::endl;

    for(uint32_t n=0; n < p_.nS; ++n)
    {
        string Cj = to_string((n/p_.vwidth)*(p_.ls1*p_.vwidth) + n%p_.vwidth);
        stream << "if(" << Cj << " >= N) return;" << std::endl;
        for(uint32_t m=0; m < p_.mS; ++m)
            stream << "rC[" << m << "][" << n << "] *= alpha;" << std::endl;
        for(uint32_t m=0; m < p_.mS; ++m)
        {
            string Ci = to_string((m/p_.vwidth)*(p_.ls0*p_.vwidth) + m%p_.vwidth);
            stream << "if(" << Ci << "< M) ";
            if(has_depth)
                stream << "C[" << Ci << CSTRIDE1 << "] = rC[" << m << "][" << n << "];" << std::endl;
            else
                stream << "C[" << Ci << CSTRIDE1 << "] = rC[" << m << "][" << n << "] + ((beta != (" << sdtype << ")0)?(beta*" << "C[" << Ci << CSTRIDE1 << "]):0);" << std::endl;
        }
        if((n+1)%p_.vwidth==0){
            stream << "C += ldc*" << p_.ls1*p_.vwidth - p_.vwidth + 1 << ";" << std::endl;
        }
        else{
            stream << "C += ldc;" << std::endl;
        }

    }

    stream.dec_tab();
    stream << "}" << std::endl;

    if(has_depth)
    {
      stream << "$KERNEL void reduce" << suffix << "($SIZE_T M, $SIZE_T N, $SIZE_T D, "
                                 << "$GLOBAL " << sdtype << "* Z, $SIZE_T Zld,"
                                 << "$GLOBAL " << sdtype << "* C, $SIZE_T ldc, $SIZE_T Cstart, $SIZE_T Cstride,"
                                 << sdtype << " beta)"
                                 << std::endl;
      stream << "{" << std::endl;
      stream.inc_tab();

      stream << "C += Cstart;" << std::endl;
      stream << "for(uint32_t i = $GLOBAL_IDX_0 ;  i < M ;  i += $GLOBAL_SIZE_0)" << std::endl;
      stream << "{" << std::endl;
      stream.inc_tab();
      stream << "for(uint32_t j = $GLOBAL_IDX_1 ;  j < N ;  j += $GLOBAL_SIZE_1)" << std::endl;
      stream << "{" << std::endl;
      stream.inc_tab();
      stream << sdtype << " acc = 0;" << std::endl;
      stream << "for(uint32_t k = 0 ;  k < D ;  k++)" << std::endl;
      stream.inc_tab();
      stream << "acc += Z[i + j*Zld + k*Zld*N];" << std::endl;
      stream.dec_tab();
      stream << "C[i*Cstride + j*ldc] = acc + ((beta != (" << sdtype << ")0)?(beta*C[i*Cstride + j*ldc]):0);" << std::endl;
      stream.dec_tab();
      stream << "}" << std::endl;
      stream.dec_tab();
      stream << "}" << std::endl;

      stream.dec_tab();
      stream << "}" << std::endl;
    }

    return stream.str();

#undef VLOAD
#undef VST0RE
  }

  void gemm::enqueue_block(driver::CommandQueue & queue, int_t M, int_t N, int_t K,
                     expression_tree::node const & A, expression_tree::node const & B, expression_tree::node const & C,
                     value_scalar const & alpha, value_scalar const & beta,
                     driver::Program const & program, std::string const & suffix, runtime::execution_options_type const & options)
  {
    using tools::align;

    if(M==0 || N==0 || K==0)
      return;

    driver::backend_type backend = queue.context().backend();

    std::string gemm_name = "gemm";
    std::string reduce_name = "reduce";

    gemm_name += suffix;
    reduce_name += suffix;

    driver::Kernel gemm(program, gemm_name.c_str());
    driver::NDRange local(p_.ls0, p_.ls1, 1);
    driver::NDRange global(align(align(M,p_.mS)/p_.mS, p_.ls0), align(align(N,p_.nS)/p_.nS, p_.ls1), p_.depth);

    uint32_t current_arg = 0;

    driver::Buffer& workspace = driver::backend::workspaces::get(options.queue(queue.context()));
    gemm.setSizeArg(current_arg++, M);
    gemm.setSizeArg(current_arg++, N);
    gemm.setSizeArg(current_arg++, K);
    if(p_.depth==1)
    {
        if(backend==driver::OPENCL)
          gemm.setArg(current_arg++, C.array.handle.cl);
        else
          gemm.setArg(current_arg++, C.array.handle.cu);
        gemm.setSizeArg(current_arg++, C.ld[1]);
        gemm.setSizeArg(current_arg++, C.array.start);
        gemm.setSizeArg(current_arg++, C.ld[0]);
    }
    else
    {
        gemm.setArg(current_arg++, workspace);
        gemm.setSizeArg(current_arg++, M);
        gemm.setSizeArg(current_arg++, 0);
        gemm.setSizeArg(current_arg++, 1);
    }


    gemm.setArg(current_arg++, alpha);
    if(backend==driver::OPENCL)
      gemm.setArg(current_arg++, A.array.handle.cl);
    else
      gemm.setArg(current_arg++, A.array.handle.cu);
    gemm.setSizeArg(current_arg++, A.ld[1]);
    gemm.setSizeArg(current_arg++, A.array.start);
    gemm.setSizeArg(current_arg++, A.ld[0]);

    if(backend==driver::OPENCL)
      gemm.setArg(current_arg++, B.array.handle.cl);
    else
      gemm.setArg(current_arg++, B.array.handle.cu);
    gemm.setSizeArg(current_arg++, B.ld[1]);
    gemm.setSizeArg(current_arg++, B.array.start);
    gemm.setSizeArg(current_arg++, B.ld[0]);

    gemm.setArg(current_arg++, beta);
    options.enqueue(program.context(), gemm, global, local);

    if(p_.depth > 1)
    {
      uint32_t current_arg = 0;
      driver::Kernel reduce(program, reduce_name.c_str());
      driver::NDRange local(p_.ls0, p_.ls1);
      driver::NDRange global(align(M, p_.ls0), align(N, p_.ls1));
      reduce.setSizeArg(current_arg++, M);
      reduce.setSizeArg(current_arg++, N);
      reduce.setSizeArg(current_arg++, p_.depth);
      reduce.setArg(current_arg++, workspace);
      reduce.setSizeArg(current_arg++, M);
      if(backend==driver::OPENCL)
        reduce.setArg(current_arg++, C.array.handle.cl);
      else
        reduce.setArg(current_arg++, C.array.handle.cu);
      reduce.setSizeArg(current_arg++, C.ld[1]);
      reduce.setSizeArg(current_arg++, C.array.start);
      reduce.setSizeArg(current_arg++, C.ld[0]);
      reduce.setArg(current_arg++, beta);
      options.enqueue(program.context(), reduce, global, local);
    }

  }

  std::vector<int_t> gemm::infos(expression_tree const & tree, symbolic::preset::gemm::args& arguments) const
  {
    expression_tree::data_type const & array = tree.data();
    std::size_t root = tree.root();
    arguments = symbolic::preset::gemm::check(array, root);
    int_t M = arguments.C->shape[0];
    int_t N = arguments.C->shape[1];
    int_t K = (A_trans_=='T')?arguments.A->shape[0]:arguments.A->shape[1];
    return {M, N, K};
  }

  gemm::gemm(uint32_t vwidth
             ,int_t ls0, int_t kL, int_t ls1, int_t D
             ,int_t ms, int_t ks, int_t ns
             ,fetch_type Afetch , fetch_type Bfetch
             ,int_t lf0, int_t lf1, char A_trans, char B_trans) :
    base_impl(vwidth, ls0, ls1), kL_(kL), depth_(D), mS_(ms), kS_(ks), nS_(ns),
    Afetch_(Afetch), Bfetch_(Bfetch), lf0_(lf0), lf1_(lf1), A_trans_(A_trans), B_trans_(B_trans)
  {
    if(A_trans_=='N' && B_trans_=='N') type_ = GEMM_NN;
    else if(A_trans_=='T' && B_trans_=='N') type_ = GEMM_TN;
    else if(A_trans_=='N' && B_trans_=='T') type_ = GEMM_NT;
    else if(A_trans_=='T' && B_trans_=='T') type_ = GEMM_TT;
    else throw;
  }

  std::vector<int_t> gemm::input_sizes(expression_tree const & expressions) const
  {
    symbolic::preset::gemm::args dummy;
    return infos((expression_tree&)expressions, dummy);
  }

  void gemm::enqueue(driver::CommandQueue & queue, driver::Program const & program, std::string const & suffix, runtime::execution_handler const & control)
  {
    expression_tree const & expressions = control.x();
    symbolic::preset::gemm::args args;
    std::vector<int_t> MNK = infos(expressions, args);
    int_t M = MNK[0];
    int_t N = MNK[1];
    int_t K = MNK[2];
    //Skip if empty
    if(M==0 || N == 0 || K ==0)
      return;
    //Enqueue
    runtime::execution_options_type const & options = control.execution_options();
    enqueue_block(queue,  M, N, K, *args.A, *args.B, *args.C, args.alpha, args.beta, program, suffix, options);
  }

  //
  gemm_nn::gemm_nn(uint32_t vwidth
                           , int_t ls0, int_t KL, int_t ls1, int_t D
                           , int_t ms, int_t ks, int_t ns
                           , fetch_type Afetch , fetch_type Bfetch
                           , int_t lf0, int_t lf1) :
    gemm(vwidth, ls0, KL, ls1, D, ms, ks, ns, Afetch, Bfetch, lf0, lf1, 'N', 'N')
  {
  }

  //
  gemm_tn::gemm_tn(uint32_t vwidth
                           , int_t ls0, int_t KL, int_t ls1, int_t D
                           , int_t ms, int_t ks, int_t ns
                           , fetch_type Afetch , fetch_type Bfetch
                           , int_t lf0, int_t lf1) :
    gemm(vwidth, ls0, KL, ls1, D, ms, ks, ns, Afetch, Bfetch, lf0, lf1, 'T', 'N')
  { }

  //
  gemm_nt::gemm_nt(uint32_t vwidth
                           , int_t ls0, int_t KL, int_t ls1, int_t D
                           , int_t ms, int_t ks, int_t ns
                           , fetch_type Afetch , fetch_type Bfetch
                           , int_t lf0, int_t lf1) :
    gemm(vwidth, ls0, KL, ls1, D, ms, ks, ns, Afetch, Bfetch, lf0, lf1, 'N', 'T')
  { }

  //
  gemm_tt::gemm_tt(uint32_t vwidth
                           , int_t ls0, int_t KL, int_t ls1, int_t D
                           , int_t ms, int_t ks, int_t ns
                           , fetch_type Afetch , fetch_type Bfetch
                           , int_t lf0, int_t lf1) :
    gemm(vwidth, ls0, KL, ls1, D, ms, ks, ns, Afetch, Bfetch, lf0, lf1, 'T', 'T')
  { }

}
}
