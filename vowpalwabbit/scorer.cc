#include <float.h>
#include "reductions.h"

struct scorer{ vw* all; }; // for set_minmax, loss

template <bool is_learn, float (*link)(float in)>
void predict_or_learn(scorer& s, LEARNER::base_learner& base, example& ec)
{
  s.all->set_minmax(s.all->sd, ec.l.simple.label);
  
  if (is_learn && ec.l.simple.label != FLT_MAX && ec.l.simple.weight > 0)
    base.learn(ec);
  else
    base.predict(ec);
  
  if(ec.l.simple.weight > 0 && ec.l.simple.label != FLT_MAX)
    ec.loss = s.all->loss->getLoss(s.all->sd, ec.pred.scalar, ec.l.simple.label) * ec.l.simple.weight;
  
  ec.pred.scalar = link(ec.pred.scalar);
}

template <float (*link)(float in)>
inline void multipredict(scorer& s, LEARNER::base_learner& base, example& ec, size_t count, size_t step, polyprediction*pred, bool finalize_predictions) {
  base.multipredict(ec, 0, count, pred, finalize_predictions); // TODO: need to thread step through???
  for (size_t c=0; c<count; c++)
    pred[c].scalar = link(pred[c].scalar);
}

void update(scorer& s, LEARNER::base_learner& base, example& ec) {
  s.all->set_minmax(s.all->sd, ec.l.simple.label);  
  base.update(ec);
}

// y = f(x) -> [0, 1]
inline float logistic(float in) { return 1.f / (1.f + exp(- in)); }

// http://en.wikipedia.org/wiki/Generalized_logistic_curve
// where the lower & upper asymptotes are -1 & 1 respectively
// 'glf1' stands for 'Generalized Logistic Function with [-1,1] range'
//    y = f(x) -> [-1, 1]
inline float glf1(float in) { return 2.f / (1.f + exp(- in)) - 1.f; }

inline float id(float in) { return in; }

LEARNER::base_learner* scorer_setup(vw& all)
{
  new_options(all)
    ("link", po::value<string>()->default_value("identity"), "Specify the link function: identity, logistic or glf1");
  add_options(all);
  po::variables_map& vm = all.vm;
  scorer& s = calloc_or_die<scorer>();
  s.all = &all;
  
  LEARNER::base_learner* base = setup_base(all);
  LEARNER::learner<scorer>* l;
  void (*multipredict_f)(scorer&, LEARNER::base_learner&, example&, size_t, size_t, polyprediction*, bool) = multipredict<id>;
  
  string link = vm["link"].as<string>();
  if (!vm.count("link") || link.compare("identity") == 0)
    l = &init_learner(&s, base, predict_or_learn<true, id>, predict_or_learn<false, id>);
  else if (link.compare("logistic") == 0)
    {
      *all.file_options << " --link=logistic ";
      l = &init_learner(&s, base, predict_or_learn<true, logistic>, 
			predict_or_learn<false, logistic>);
      multipredict_f = multipredict<logistic>;
    }
  else if (link.compare("glf1") == 0)
    {
      *all.file_options << " --link=glf1 ";
      l = &init_learner(&s, base, predict_or_learn<true, glf1>, 
			predict_or_learn<false, glf1>);
      multipredict_f = multipredict<glf1>;
    }
  else
    {
      cerr << "Unknown link function: " << link << endl;
      throw exception();
    }
  l->set_multipredict(multipredict_f);
  l->set_update(update);
  all.scorer = make_base(*l);
  
  return all.scorer;
}
