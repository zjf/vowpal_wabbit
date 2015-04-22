/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <sstream>
#include <float.h>
#include "reductions.h"
#include "rand48.h"

struct oaa {
  size_t k;
  vw* all; // for raw
  polyprediction* pred;  // for multipredict
  size_t num_subsample; // for randomized subsampling, how many negatives to draw?
  uint32_t* subsample_order; // for randomized subsampling, in what order should we touch classes
  size_t subsample_id; // for randomized subsampling, where do we live in the list
};

void learn_randomized(oaa& o, LEARNER::base_learner& base, example& ec) {
  MULTICLASS::label_t ld = ec.l.multi;
  if (ld.label == 0 || (ld.label > o.k && ld.label != (uint32_t)-1))
    cout << "label " << ld.label << " is not in {1,"<< o.k << "} This won't work right." << endl;

  stringstream outputStringStream;
  uint32_t prediction = 1;

  ec.l.simple = { 1., ld.weight, 0.f }; // truth
  base.learn(ec, ld.label-1);

  ec.l.simple.label = -1.;
  size_t p = o.subsample_id;
  o.subsample_id = (o.subsample_id + o.num_subsample) % (o.k-1);
  for (size_t c=0; c<o.num_subsample; c++) {
    uint32_t l = o.subsample_order[p];
    if (l >= ld.label-1) l++;
    base.learn(ec, l);
    p = (p+1) % (o.k-1);
  }

  ec.pred.multiclass = prediction;
  ec.l.multi = ld;
}

template <bool is_learn, bool print_all>
void predict_or_learn(oaa& o, LEARNER::base_learner& base, example& ec) {
  MULTICLASS::label_t mc_label_data = ec.l.multi;
  if (mc_label_data.label == 0 || (mc_label_data.label > o.k && mc_label_data.label != (uint32_t)-1))
    cout << "label " << mc_label_data.label << " is not in {1,"<< o.k << "} This won't work right." << endl;

  stringstream outputStringStream;
  uint32_t prediction = 1;

  ec.l.simple = { FLT_MAX, mc_label_data.weight, 0.f };
  base.multipredict(ec, 0, o.k, o.pred, true);
  for (uint32_t i=2; i<=o.k; i++)
    if (o.pred[i-1].scalar > o.pred[prediction-1].scalar)
      prediction = i;
  
  if (is_learn) {
    for (uint32_t i=1; i<=o.k; i++) {
      ec.l.simple = { (mc_label_data.label == i) ? 1.f : -1.f, mc_label_data.weight, 0.f };
      ec.pred.scalar = o.pred[i-1].scalar;
      base.update(ec, i-1);
    }
  } 

  if (print_all) {
    outputStringStream << "1:" << o.pred[0].scalar;
    for (uint32_t i=2; i<=o.k; i++) outputStringStream << ' ' << i << ':' << o.pred[i-1].scalar;
    o.all->print_text(o.all->raw_prediction, outputStringStream.str(), ec.tag);
  }
  
  ec.pred.multiclass = prediction;
  ec.l.multi = mc_label_data;
}

void finish(oaa&o) { free(o.pred); free(o.subsample_order); }

LEARNER::base_learner* oaa_setup(vw& all)
{
  if (missing_option<size_t, true>(all, "oaa", "One-against-all multiclass with <k> labels")) 
    return nullptr;
  new_options(all, "oaa options")
      ("oaa_subsample", po::value<size_t>(), "subsample this number of negative examples when learning");
  
  oaa& data = calloc_or_die<oaa>();
  data.k = all.vm["oaa"].as<size_t>();
  data.all = &all;
  data.pred = calloc_or_die<polyprediction>(data.k);
  data.num_subsample = 0; data.subsample_order = NULL; data.subsample_id = 0;
  if (all.vm.count("oaa_subsample")) {
    data.num_subsample = all.vm["oaa_subsample"].as<size_t>();
    if (data.num_subsample >= data.k) {
      data.num_subsample = 0;
      cerr << "oaa is turning off subsampling because your parameter >= K" << endl;
    } else {
      data.subsample_order = calloc_or_die<uint32_t>(data.k-1);
      for (size_t i=0; i<data.k-1; i++) data.subsample_order[i] = i;
      for (size_t i=0; i<data.k-1; i++) {
        size_t j = (size_t)(frand48() * (float)(data.k-i)) + i;
        uint32_t tmp = data.subsample_order[i];
        data.subsample_order[i] = data.subsample_order[j];
        data.subsample_order[j] = tmp;
      }
    }
  }
  
  LEARNER::learner<oaa>* l;
  if (all.raw_prediction > 0)
    l = &LEARNER::init_multiclass_learner(&data, setup_base(all), predict_or_learn<true, true>, 
					  predict_or_learn<false, true>, all.p, data.k);
  else
    l = &LEARNER::init_multiclass_learner(&data, setup_base(all),predict_or_learn<true, false>, 
					  predict_or_learn<false, false>, all.p, data.k);
  l->set_finish(finish);
  
  return make_base(*l);
}
