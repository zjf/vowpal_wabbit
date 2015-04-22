/*
  Copyright (c) by respective owners including Yahoo!, Microsoft, and
  individual contributors. All rights reserved.  Released under a BSD
  license as described in the file LICENSE.
*/
#pragma once
#ifdef __FreeBSD__
#include <sys/socket.h>
#endif

#include "parse_regressor.h"
#include "constant.h"

namespace GD{
  LEARNER::base_learner* setup(vw& all);

  struct gd;

  float finalize_prediction(shared_data* sd, float ret);
  void print_audit_features(vw&, example& ec);
  void save_load_regressor(vw& all, io_buf& model_file, bool read, bool text);
  void save_load_online_state(vw& all, io_buf& model_file, bool read, bool text, GD::gd *g = nullptr);

  struct multipredict_info { size_t count; size_t step; polyprediction* pred; regressor* reg; /* & for l1: */ float gravity; };

  inline void vec_add_multipredict(multipredict_info& mp, const float fx, uint32_t fi) {
    if ((-1e-10 < fx) && (fx < 1e-10)) return;
    weight*w    = mp.reg->weight_vector;
    size_t mask = mp.reg->weight_mask;
    polyprediction* p = mp.pred;

    fi &= mask;
    uint32_t top = fi + (mp.count-1) * mp.step;
    if (top <= mask) {
      weight* last = w + top;
      w += fi;
      for (; w <= last; w += mp.step, ++p)
        p->scalar += fx * *w;
    } else  // TODO: this could be faster by unrolling into two loops
      for (size_t c=0; c<mp.count; ++c, fi += mp.step, ++p) {
        fi &= mask;
        p->scalar += fx * w[fi];
      }
  }
  
  // iterate through one namespace (or its part), callback function T(some_data_R, feature_value_x, feature_weight)
  template <class R, void (*T)(R&, const float, float&)>
  inline void foreach_feature(weight* weight_vector, size_t weight_mask, feature* begin, feature* end, R& dat, uint32_t offset=0, float mult=1.)
  {
    for (feature* f = begin; f!= end; f++)
      T(dat, mult*f->x, weight_vector[(f->weight_index + offset) & weight_mask]);
  }

  // iterate through one namespace (or its part), callback function T(some_data_R, feature_value_x, feature_index)
  template <class R, void (*T)(R&, float, uint32_t)>
   void foreach_feature(weight* weight_vector, size_t weight_mask, feature* begin, feature* end, R&dat, uint32_t offset=0, float mult=1.)
   {
     for (feature* f = begin; f!= end; f++)
       T(dat, mult*f->x, f->weight_index + offset);
   }
 
  // iterate through all namespaces and quadratic&cubic features, callback function T(some_data_R, feature_value_x, S)
  // where S is EITHER float& feature_weight OR uint32_t feature_index
  template <class R, class S, void (*T)(R&, float, S)>
  inline void foreach_feature(vw& all, example& ec, R& dat)
  {
    uint32_t offset = ec.ft_offset;

    for (unsigned char* i = ec.indices.begin; i != ec.indices.end; i++)
      foreach_feature<R,T>(all.reg.weight_vector, all.reg.weight_mask, ec.atomics[*i].begin, ec.atomics[*i].end, dat, offset);
     
    for (vector<string>::iterator i = all.pairs.begin(); i != all.pairs.end();i++) {
      if (ec.atomics[(unsigned char)(*i)[0]].size() > 0) {
        v_array<feature> temp = ec.atomics[(unsigned char)(*i)[0]];
        for (; temp.begin != temp.end; temp.begin++)
        {
          uint32_t halfhash = quadratic_constant * (temp.begin->weight_index) + offset;
       
          foreach_feature<R,T>(all.reg.weight_vector, all.reg.weight_mask, ec.atomics[(unsigned char)(*i)[1]].begin, ec.atomics[(unsigned char)(*i)[1]].end, dat, 
                               halfhash, temp.begin->x);
        }
      }
    }

    for (vector<string>::iterator i = all.triples.begin(); i != all.triples.end();i++) {
      if ((ec.atomics[(unsigned char)(*i)[0]].size() == 0) || (ec.atomics[(unsigned char)(*i)[1]].size() == 0) || (ec.atomics[(unsigned char)(*i)[2]].size() == 0)) { continue; }
      v_array<feature> temp1 = ec.atomics[(unsigned char)(*i)[0]];
      for (; temp1.begin != temp1.end; temp1.begin++) {
        v_array<feature> temp2 = ec.atomics[(unsigned char)(*i)[1]];
        for (; temp2.begin != temp2.end; temp2.begin++) {

          uint32_t a = temp1.begin->weight_index;
          uint32_t b = temp2.begin->weight_index;
          uint32_t halfhash = cubic_constant2 * (cubic_constant * a + b) + offset;
          float mult = temp1.begin->x * temp2.begin->x;
          foreach_feature<R,T>(all.reg.weight_vector, all.reg.weight_mask, ec.atomics[(unsigned char)(*i)[2]].begin, ec.atomics[(unsigned char)(*i)[2]].end, dat, halfhash, mult);
        }
      }
    }
  }

  // iterate through all namespaces and quadratic&cubic features, callback function T(some_data_R, feature_value_x, feature_weight)
  template <class R, void (*T)(R&, float, float&)>
  inline void foreach_feature(vw& all, example& ec, R& dat)
  {
    foreach_feature<R,float&,T>(all, ec, dat);
  }

 inline void vec_add(float& p, const float fx, float& fw) { p += fw * fx; }

  inline float inline_predict(vw& all, example& ec)
  {
    float temp = ec.l.simple.initial;
    foreach_feature<float, vec_add>(all, ec, temp);
    return temp;
  }
}
