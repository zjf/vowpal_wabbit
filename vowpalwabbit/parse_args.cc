/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <stdio.h>
#include <float.h>
#include <sstream>
#include <fstream>

#include "parse_regressor.h"
#include "parser.h"
#include "vw.h"

#include "sender.h"
#include "nn.h"
#include "gd.h"
#include "cbify.h"
#include "oaa.h"
#include "multilabel_oaa.h"
#include "rand48.h"
#include "bs.h"
#include "topk.h"
#include "ect.h"
#include "csoaa.h"
#include "cb_algs.h"
#include "scorer.h"
#include "search.h"
#include "bfgs.h"
#include "lda_core.h"
#include "noop.h"
#include "print.h"
#include "gd_mf.h"
#include "learner.h"
#include "mf.h"
#include "ftrl.h"
#include "svrg.h"
#include "rand48.h"
#include "binary.h"
#include "lrq.h"
#include "lrqfa.h"
#include "autolink.h"
#include "log_multi.h"
#include "stagewise_poly.h"
#include "active.h"
#include "kernel_svm.h"
#include "parse_example.h"
#include "best_constant.h"

using namespace std;
//
// Does string end with a certain substring?
//
bool ends_with(string const &fullString, string const &ending)
{
    if (fullString.length() > ending.length()) {
        return (fullString.compare(fullString.length() - ending.length(), ending.length(), ending) == 0);
    } else {
        return false;
    }
}

bool valid_ns(char c)
{
    if (c=='|'||c==':')
        return false;
    return true;
}

bool substring_equal(substring&a, substring&b) {
  return (a.end - a.begin == b.end - b.begin) // same length
      && (strncmp(a.begin, b.begin, a.end - a.begin) == 0);
}  

void parse_dictionary_argument(vw&all, string str) {
  if (str.length() == 0) return;
  // expecting 'namespace:file', for instance 'w:foo.txt'
  // in the case of just 'foo.txt' it's applied to the default namespace

  char ns = ' ';
  const char*s  = str.c_str();
  if ((str.length() > 3) && (str[1] == ':')) {
    ns = str[0];
    s  += 2;
  }

  // see if we've already read this dictionary
  for (size_t id=0; id<all.read_dictionaries.size(); id++)
    if (strcmp(all.read_dictionaries[id].name, s) == 0) {
      all.namespace_dictionaries[(size_t)ns].push_back(all.read_dictionaries[id].dict);
      return;
    }

  feature_dict* map = new feature_dict(1023, nullptr, substring_equal);
  
  // TODO: handle gzipped dictionaries
  example *ec = alloc_examples(all.p->lp.label_size, 1);
  ifstream infile(s);
  size_t def = (size_t)' ';
  for (string line; getline(infile, line);) {
    char* c = (char*)line.c_str(); // we're throwing away const, which is dangerous...
    while (*c == ' ' || *c == '\t') ++c; // skip initial whitespace
    char* d = c;
    while (*d != ' ' && *d != '\t' && *d != '\n' && *d != '\0') ++d; // gobble up initial word
    if (d == c) continue; // no word
    if (*d != ' ' && *d != '\t') continue; // reached end of line
    char* word = calloc_or_die<char>(d-c);
    memcpy(word, c, d-c);
    substring ss = { word, word + (d - c) };
    uint32_t hash = uniform_hash( ss.begin, ss.end-ss.begin, quadratic_constant);
    if (map->get(ss, hash) != nullptr) { // don't overwrite old values!
      free(word);
      continue;
    }
    
    d--;
    *d = '|';  // set up for parser::read_line
    read_line(all, ec, d);
    // now we just need to grab stuff from the default namespace of ec!
    if (ec->atomics[def].size() == 0) {
      free(word);
      continue;
    }
    v_array<feature>* arr = new v_array<feature>;
    *arr = v_init<feature>();
    push_many(*arr, ec->atomics[def].begin, ec->atomics[def].size());
    map->put(ss, hash, arr);

    // clear up ec
    ec->tag.erase(); ec->indices.erase();
    for (size_t i=0; i<256; i++) { ec->atomics[i].erase(); ec->audit_features[i].erase(); }
  }
  dealloc_example(all.p->lp.delete_label, *ec);
  free(ec);
  
  cerr << "dictionary " << s << " contains " << map->size() << " item" << (map->size() == 1 ? "\n" : "s\n");
  all.namespace_dictionaries[(size_t)ns].push_back(map);
  dictionary_info info = { calloc_or_die<char>(strlen(s)+1), map };
  strcpy(info.name, s);
  all.read_dictionaries.push_back(info);
}

void parse_affix_argument(vw&all, string str) {
  if (str.length() == 0) return;
  char* cstr = calloc_or_die<char>(str.length()+1);
  strcpy(cstr, str.c_str());

  char*p = strtok(cstr, ",");
  while (p != 0) {
    char*q = p;
    uint16_t prefix = 1;
    if (q[0] == '+') { q++; }
    else if (q[0] == '-') { prefix = 0; q++; }
    if ((q[0] < '1') || (q[0] > '7')) {
      cerr << "malformed affix argument (length must be 1..7): " << p << endl;
      throw exception();
    }
    uint16_t len = (uint16_t)(q[0] - '0');
    uint16_t ns = (uint16_t)' ';  // default namespace
    if (q[1] != 0) {
      if (valid_ns(q[1]))
        ns = (uint16_t)q[1];
      else {
        cerr << "malformed affix argument (invalid namespace): " << p << endl;
        throw exception();
      }
      if (q[2] != 0) {
        cerr << "malformed affix argument (too long): " << p << endl;
        throw exception();
      }
    }

    uint16_t afx = (len << 1) | (prefix & 0x1);
    all.affix_features[ns] <<= 4;
    all.affix_features[ns] |=  afx;

    p = strtok(nullptr, ",");
  }

  free(cstr);
}

void parse_diagnostics(vw& all, int argc)
{
  new_options(all, "Diagnostic options")
    ("version","Version information")
    ("audit,a", "print weights of features")
    ("progress,P", po::value< string >(), "Progress update frequency. int: additive, float: multiplicative")
    ("quiet", "Don't output disgnostics and progress updates")
    ("help,h","Look here: http://hunch.net/~vw/ and click on Tutorial.");
  add_options(all);

  po::variables_map& vm = all.vm;

  if (vm.count("version")) {
    /* upon direct query for version -- spit it out to stdout */
    cout << version.to_string() << "\n";
    exit(0);
  }

  if (vm.count("quiet")) {
    all.quiet = true;
    // --quiet wins over --progress
  } else {
    if (argc == 1)
      cerr << "For more information use: vw --help" << endl;

    all.quiet = false;

    if (vm.count("progress")) {
      string progress_str = vm["progress"].as<string>();
      all.progress_arg = (float)::atof(progress_str.c_str());

      // --progress interval is dual: either integer or floating-point
      if (progress_str.find_first_of(".") == string::npos) {
        // No "." in arg: assume integer -> additive
        all.progress_add = true;
        if (all.progress_arg < 1) {
          cerr    << "warning: additive --progress <int>"
                  << " can't be < 1: forcing to 1\n";
          all.progress_arg = 1;

        }
        all.sd->dump_interval = all.progress_arg;

      } else {
        // A "." in arg: assume floating-point -> multiplicative
        all.progress_add = false;

        if (all.progress_arg <= 1.0) {
          cerr    << "warning: multiplicative --progress <float>: "
                  << vm["progress"].as<string>()
                  << " is <= 1.0: adding 1.0\n";
          all.progress_arg += 1.0;

        } else if (all.progress_arg > 9.0) {
          cerr    << "warning: multiplicative --progress <float>"
                  << " is > 9.0: you probably meant to use an integer\n";
        }
        all.sd->dump_interval = 1.0;
      }
    }
  }  

  if (vm.count("audit")){
    all.audit = true;
  }
}

void parse_source(vw& all)
{
  new_options(all, "Input options")
    ("data,d", po::value< string >(), "Example Set")
    ("daemon", "persistent daemon mode on port 26542")
    ("port", po::value<size_t>(),"port to listen on; use 0 to pick unused port")
    ("num_children", po::value<size_t>(&(all.num_children)), "number of children for persistent daemon mode")
    ("pid_file", po::value< string >(), "Write pid file in persistent daemon mode")
    ("port_file", po::value< string >(), "Write port used in persistent daemon mode")
    ("cache,c", "Use a cache.  The default is <data>.cache")
    ("cache_file", po::value< vector<string> >(), "The location(s) of cache_file.")
    ("kill_cache,k", "do not reuse existing cache: create a new one always")
    ("compressed", "use gzip format whenever possible. If a cache file is being created, this option creates a compressed cache file. A mixture of raw-text & compressed inputs are supported with autodetection.")
    ("no_stdin", "do not default to reading from stdin");
  add_options(all);

  // Be friendly: if -d was left out, treat positional param as data file
  po::positional_options_description p;  
  p.add("data", -1);
  
  po::parsed_options pos = po::command_line_parser(all.args).
    style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing).
    options(all.opts).positional(p).run();
  all.vm = po::variables_map();
  po::store(pos, all.vm);
  po::variables_map& vm = all.vm;
 
  //begin input source
  if (vm.count("no_stdin"))
    all.stdin_off = true;
  
  if ( (vm.count("total") || vm.count("node") || vm.count("unique_id")) && !(vm.count("total") && vm.count("node") && vm.count("unique_id")) )
    {
      cout << "you must specificy unique_id, total, and node if you specify any" << endl;
      throw exception();
    }
  
  if (vm.count("daemon") || vm.count("pid_file") || (vm.count("port") && !all.active) ) {
    all.daemon = true;
    
    // allow each child to process up to 1e5 connections
    all.numpasses = (size_t) 1e5;
  }

  if (vm.count("compressed"))
      set_compressed(all.p);

  if (vm.count("data")) {
    all.data_filename = vm["data"].as<string>();
    if (ends_with(all.data_filename, ".gz"))
      set_compressed(all.p);
  } else
    all.data_filename = "";

  if ((vm.count("cache") || vm.count("cache_file")) && vm.count("invert_hash"))
    {
      cout << "invert_hash is incompatible with a cache file.  Use it in single pass mode only." << endl;
      throw exception();
    }

  if(!all.holdout_set_off && (vm.count("output_feature_regularizer_binary") || vm.count("output_feature_regularizer_text")))
    {
      all.holdout_set_off = true;
      cerr<<"Making holdout_set_off=true since output regularizer specified\n";
    }
}

void parse_feature_tweaks(vw& all)
{
  new_options(all, "Feature options")
    ("hash", po::value< string > (), "how to hash the features. Available options: strings, all")
    ("ignore", po::value< vector<unsigned char> >(), "ignore namespaces beginning with character <arg>")
    ("keep", po::value< vector<unsigned char> >(), "keep namespaces beginning with character <arg>")
    ("redefine", po::value< vector<string> >(), "redefine namespaces beginning with characters of string S as namespace N. <arg> shall be in form 'N:=S' where := is operator. Empty N or S are treated as default namespace. Use ':' as a wildcard in S.")
    ("bit_precision,b", po::value<size_t>(), "number of bits in the feature table")
    ("noconstant", "Don't add a constant feature")
    ("constant,C", po::value<float>(&(all.initial_constant)), "Set initial value of constant")
    ("ngram", po::value< vector<string> >(), "Generate N grams. To generate N grams for a single namespace 'foo', arg should be fN.")
    ("skips", po::value< vector<string> >(), "Generate skips in N grams. This in conjunction with the ngram tag can be used to generate generalized n-skip-k-gram. To generate n-skips for a single namespace 'foo', arg should be fN.")
    ("feature_limit", po::value< vector<string> >(), "limit to N features. To apply to a single namespace 'foo', arg should be fN")
    ("affix", po::value<string>(), "generate prefixes/suffixes of features; argument '+2a,-3b,+1' means generate 2-char prefixes for namespace a, 3-char suffixes for b and 1 char prefixes for default namespace")
    ("spelling", po::value< vector<string> >(), "compute spelling features for a give namespace (use '_' for default namespace)")
    ("dictionary", po::value< vector<string> >(), "read a dictionary for additional features (arg either 'x:file' or just 'file')")
    ("quadratic,q", po::value< vector<string> > (), "Create and use quadratic features")
    ("q:", po::value< string >(), ": corresponds to a wildcard for all printable characters")
    ("cubic", po::value< vector<string> > (),
     "Create and use cubic features");
  add_options(all);

  po::variables_map& vm = all.vm;

  //feature manipulation
  string hash_function("strings");
  if(vm.count("hash")) 
    hash_function = vm["hash"].as<string>();
  all.p->hasher = getHasher(hash_function);
      
  if (vm.count("spelling")) {
    vector<string> spelling_ns = vm["spelling"].as< vector<string> >();
    for (size_t id=0; id<spelling_ns.size(); id++)
      if (spelling_ns[id][0] == '_') all.spelling_features[(unsigned char)' '] = true;
      else all.spelling_features[(size_t)spelling_ns[id][0]] = true;
  }

  if (vm.count("affix")) {
    parse_affix_argument(all, vm["affix"].as<string>());
    *all.file_options << " --affix " << vm["affix"].as<string>();
  }

  if(vm.count("ngram")){
    if(vm.count("sort_features"))
      {
	cerr << "ngram is incompatible with sort_features.  " << endl;
	throw exception();
      }

    all.ngram_strings = vm["ngram"].as< vector<string> >();
    compile_gram(all.ngram_strings, all.ngram, (char*)"grams", all.quiet);
  }

  if(vm.count("skips"))
    {
      if(!vm.count("ngram"))
	{
	  cout << "You can not skip unless ngram is > 1" << endl;
	  throw exception();
	}

      all.skip_strings = vm["skips"].as<vector<string> >();
      compile_gram(all.skip_strings, all.skips, (char*)"skips", all.quiet);
    }

  if(vm.count("feature_limit"))
    {
      all.limit_strings = vm["feature_limit"].as< vector<string> >();
      compile_limits(all.limit_strings, all.limit, all.quiet);
    }

  if (vm.count("bit_precision"))
    {
      uint32_t new_bits = (uint32_t)vm["bit_precision"].as< size_t>();
      if (all.default_bits == false && new_bits != all.num_bits)
	{
	  cout << "Number of bits is set to " << new_bits << " and " << all.num_bits << " by argument and model.  That does not work." << endl;
	  throw exception();
	}
      all.default_bits = false;
      all.num_bits = new_bits;
      if (all.num_bits > min(31, sizeof(size_t)*8 - 3))
	{
	  cout << "Only " << min(31, sizeof(size_t)*8 - 3) << " or fewer bits allowed.  If this is a serious limit, speak up." << endl;
	  throw exception();
	}
    }

  if (vm.count("quadratic"))
    {
      all.pairs = vm["quadratic"].as< vector<string> >();
      vector<string> newpairs;
      //string tmp;
      char printable_start = '!';
      char printable_end = '~';
      int valid_ns_size = printable_end - printable_start - 1; //will skip two characters

      if(!all.quiet)
        cerr<<"creating quadratic features for pairs: ";

      for (vector<string>::iterator i = all.pairs.begin(); i != all.pairs.end();i++){
        if(!all.quiet){
          cerr << *i << " ";
          if (i->length() > 2)
            cerr << endl << "warning, ignoring characters after the 2nd.\n";
          if (i->length() < 2) {
            cerr << endl << "error, quadratic features must involve two sets.\n";
            throw exception();
          }
        }
        //-q x:
        if((*i)[0]!=':'&&(*i)[1]==':'){
          newpairs.reserve(newpairs.size() + valid_ns_size);
          for (char j=printable_start; j<=printable_end; j++){
            if(valid_ns(j))
              newpairs.push_back(string(1,(*i)[0])+j);
          }
        }
        //-q :x
        else if((*i)[0]==':'&&(*i)[1]!=':'){
          newpairs.reserve(newpairs.size() + valid_ns_size);
          for (char j=printable_start; j<=printable_end; j++){
            if(valid_ns(j)){
	      stringstream ss;
	      ss << j << (*i)[1];
	      newpairs.push_back(ss.str());
	    }
          }
        }
        //-q ::
        else if((*i)[0]==':'&&(*i)[1]==':'){
	  cout << "in pair creation" << endl;
          newpairs.reserve(newpairs.size() + valid_ns_size*valid_ns_size);
	  stringstream ss;
	  ss << ' ' << ' ';
	  newpairs.push_back(ss.str());
          for (char j=printable_start; j<=printable_end; j++){
            if(valid_ns(j)){
              for (char k=printable_start; k<=printable_end; k++){
                if(valid_ns(k)){
		  stringstream ss;
                  ss << j << k;
                  newpairs.push_back(ss.str());
		}
              }
            }
          }
        }
        else{
          newpairs.push_back(string(*i));
        }
      }
      newpairs.swap(all.pairs);
      if(!all.quiet)
        cerr<<endl;
    }

  if (vm.count("cubic"))
    {
      all.triples = vm["cubic"].as< vector<string> >();
      if (!all.quiet)
	{
	  cerr << "creating cubic features for triples: ";
	  for (vector<string>::iterator i = all.triples.begin(); i != all.triples.end();i++) {
	    cerr << *i << " ";
	    if (i->length() > 3)
	      cerr << endl << "warning, ignoring characters after the 3rd.\n";
	    if (i->length() < 3) {
	      cerr << endl << "error, cubic features must involve three sets.\n";
	      throw exception();
	    }
	  }
	  cerr << endl;
	}
    }

  for (size_t i = 0; i < 256; i++)
    all.ignore[i] = false;
  all.ignore_some = false;
  
  if (vm.count("ignore"))
    {
      all.ignore_some = true;

      vector<unsigned char> ignore = vm["ignore"].as< vector<unsigned char> >();
      for (vector<unsigned char>::iterator i = ignore.begin(); i != ignore.end();i++)
	{
	  all.ignore[*i] = true;
	}
      if (!all.quiet)
	{
	  cerr << "ignoring namespaces beginning with: ";
	  for (vector<unsigned char>::iterator i = ignore.begin(); i != ignore.end();i++)
	    cerr << *i << " ";

	  cerr << endl;
	}
    }

  if (vm.count("keep"))
    {
      for (size_t i = 0; i < 256; i++)
        all.ignore[i] = true;

      all.ignore_some = true;

      vector<unsigned char> keep = vm["keep"].as< vector<unsigned char> >();
      for (vector<unsigned char>::iterator i = keep.begin(); i != keep.end();i++)
	{
	  all.ignore[*i] = false;
	}
      if (!all.quiet)
	{
	  cerr << "using namespaces beginning with: ";
	  for (vector<unsigned char>::iterator i = keep.begin(); i != keep.end();i++)
	    cerr << *i << " ";

	  cerr << endl;
	}
    }

  // --redefine param code
  all.redefine_some = false; // false by default

  if (vm.count("redefine"))
  {
      // initail values: i-th namespace is redefined to i itself
      for (size_t i = 0; i < 256; i++)
          all.redefine[i] = (unsigned char)i;

      // note: --redefine declaration order is matter
      // so --redefine :=L --redefine ab:=M  --ignore L  will ignore all except a and b under new M namspace

      vector< string > arg_list = vm["redefine"].as< vector< string > >();
      for (vector<string>::iterator arg_iter = arg_list.begin(); arg_iter != arg_list.end(); arg_iter++)
      {
          string arg = *arg_iter;
          size_t arg_len = arg.length();

          size_t operator_pos = 0; //keeps operator pos + 1 to stay unsigned type
          bool operator_found = false;
          unsigned char new_namespace = ' ';

          // let's find operator ':=' position in N:=S
          for (size_t i = 0; i < arg_len; i++)
          {
              if (operator_found)
              {
                  if (i > 2) { new_namespace = arg[0];} //N is not empty
                  break;
              } else
                  if (arg[i] == ':')
                      operator_pos = i+1;
                  else
                      if ( (arg[i] == '=') && (operator_pos == i) )
                          operator_found = true;
          }

          if (!operator_found)
          {
              cerr << "argument of --redefine is malformed. Valid format is N:=S, :=S or N:=" << endl;
              throw exception();
          }

          if (++operator_pos > 3) // seek operator end
              cerr << "WARNING: multiple namespaces are used in target part of --redefine argument. Only first one ('" << new_namespace << "') will be used as target namespace." << endl;

          all.redefine_some = true;         

          // case ':=S' doesn't require any additional code as new_namespace = ' ' by default

          if (operator_pos == arg_len) // S is empty, default namespace shall be used
              all.redefine[' '] = new_namespace;
          else
              for (size_t i = operator_pos; i < arg_len; i++)
              { // all namespaces from S are redefined to N
                  unsigned char c = arg[i];
                  if (c != ':')
                      all.redefine[c] = new_namespace;
                  else
                  { // wildcard found: redefine all except default and break
                      for (size_t i = 0; i < 256; i++)
                      {
                          if (i != ' ')
                              all.redefine[i] = new_namespace;
                      }
                      break; //break processing S
                  }
              }

      }
  }

  if (vm.count("dictionary")) {
    vector<string> dictionary_ns = vm["dictionary"].as< vector<string> >();
    for (size_t id=0; id<dictionary_ns.size(); id++)
      parse_dictionary_argument(all, dictionary_ns[id]);
  }
  
  if (vm.count("noconstant"))
    all.add_constant = false;
}

void parse_example_tweaks(vw& all)
{
  new_options(all, "Example options")
    ("testonly,t", "Ignore label information and just test")
    ("holdout_off", "no holdout data in multiple passes")
    ("holdout_period", po::value<uint32_t>(&(all.holdout_period)), "holdout period for test only, default 10")
    ("holdout_after", po::value<uint32_t>(&(all.holdout_after)), "holdout after n training examples, default off (disables holdout_period)")
    ("early_terminate", po::value<size_t>(), "Specify the number of passes tolerated when holdout loss doesn't decrease before early termination, default is 3")
    ("passes", po::value<size_t>(&(all.numpasses)),"Number of Training Passes")
    ("initial_pass_length", po::value<size_t>(&(all.pass_length)), "initial number of examples per pass")
    ("examples", po::value<size_t>(&(all.max_examples)), "number of examples to parse")
    ("min_prediction", po::value<float>(&(all.sd->min_label)), "Smallest prediction to output")
    ("max_prediction", po::value<float>(&(all.sd->max_label)), "Largest prediction to output")
    ("sort_features", "turn this on to disregard order in which features have been defined. This will lead to smaller cache sizes")
    ("loss_function", po::value<string>()->default_value("squared"), "Specify the loss function to be used, uses squared by default. Currently available ones are squared, classic, hinge, logistic and quantile.")
    ("quantile_tau", po::value<float>()->default_value(0.5), "Parameter \\tau associated with Quantile loss. Defaults to 0.5")
    ("l1", po::value<float>(&(all.l1_lambda)), "l_1 lambda")
    ("l2", po::value<float>(&(all.l2_lambda)), "l_2 lambda");
  add_options(all);

  po::variables_map& vm = all.vm;
  if (vm.count("testonly") || all.eta == 0.)
    {
      if (!all.quiet)
	cerr << "only testing" << endl;
      all.training = false;
      if (all.lda > 0)
        all.eta = 0;
    }
  else
    all.training = true;

  if(all.numpasses > 1)
      all.holdout_set_off = false;

  if(vm.count("holdout_off"))
      all.holdout_set_off = true;

  if(vm.count("sort_features"))
    all.p->sort_features = true;
  
  if (vm.count("min_prediction"))
    all.sd->min_label = vm["min_prediction"].as<float>();
  if (vm.count("max_prediction"))
    all.sd->max_label = vm["max_prediction"].as<float>();
  if (vm.count("min_prediction") || vm.count("max_prediction") || vm.count("testonly"))
    all.set_minmax = noop_mm;

  string loss_function = vm["loss_function"].as<string>();
  float loss_parameter = 0.0;
  if(vm.count("quantile_tau"))
    loss_parameter = vm["quantile_tau"].as<float>();

  all.loss = getLossFunction(all, loss_function, (float)loss_parameter);

  if (all.l1_lambda < 0.) {
    cerr << "l1_lambda should be nonnegative: resetting from " << all.l1_lambda << " to 0" << endl;
    all.l1_lambda = 0.;
  }
  if (all.l2_lambda < 0.) {
    cerr << "l2_lambda should be nonnegative: resetting from " << all.l2_lambda << " to 0" << endl;
    all.l2_lambda = 0.;
  }
  all.reg_mode += (all.l1_lambda > 0.) ? 1 : 0;
  all.reg_mode += (all.l2_lambda > 0.) ? 2 : 0;
  if (!all.quiet)
    {
      if (all.reg_mode %2 && !vm.count("bfgs"))
	cerr << "using l1 regularization = " << all.l1_lambda << endl;
      if (all.reg_mode > 1)
	cerr << "using l2 regularization = " << all.l2_lambda << endl;
    }
}

void parse_output_preds(vw& all)
{
  new_options(all, "Output options")
    ("predictions,p", po::value< string >(), "File to output predictions to")
    ("raw_predictions,r", po::value< string >(), "File to output unnormalized predictions to");
  add_options(all);

  po::variables_map& vm = all.vm;
  if (vm.count("predictions")) {
    if (!all.quiet)
      cerr << "predictions = " <<  vm["predictions"].as< string >() << endl;
    if (strcmp(vm["predictions"].as< string >().c_str(), "stdout") == 0)
      {
	all.final_prediction_sink.push_back((size_t) 1);//stdout
      }
    else
      {
	const char* fstr = (vm["predictions"].as< string >().c_str());
	int f;
#ifdef _WIN32
	_sopen_s(&f, fstr, _O_CREAT|_O_WRONLY|_O_BINARY|_O_TRUNC, _SH_DENYWR, _S_IREAD|_S_IWRITE);
#else
	f = open(fstr, O_CREAT|O_WRONLY|O_LARGEFILE|O_TRUNC,0666);
#endif
	if (f < 0)
	  cerr << "Error opening the predictions file: " << fstr << endl;
	all.final_prediction_sink.push_back((size_t) f);
      }
  }

  if (vm.count("raw_predictions")) {
    if (!all.quiet) {
      cerr << "raw predictions = " <<  vm["raw_predictions"].as< string >() << endl;
      if (vm.count("binary"))
        cerr << "Warning: --raw has no defined value when --binary specified, expect no output" << endl;
    }
    if (strcmp(vm["raw_predictions"].as< string >().c_str(), "stdout") == 0)
      all.raw_prediction = 1;//stdout
    else
	{
	  const char* t = vm["raw_predictions"].as< string >().c_str();
	  int f;
#ifdef _WIN32
	  _sopen_s(&f, t, _O_CREAT|_O_WRONLY|_O_BINARY|_O_TRUNC, _SH_DENYWR, _S_IREAD|_S_IWRITE);
#else
	  f = open(t, O_CREAT|O_WRONLY|O_LARGEFILE|O_TRUNC,0666);
#endif
	  all.raw_prediction = f;
	}
  }
}

void parse_output_model(vw& all)
{
  new_options(all, "Output model")
    ("final_regressor,f", po::value< string >(), "Final regressor")
    ("readable_model", po::value< string >(), "Output human-readable final regressor with numeric features")
    ("invert_hash", po::value< string >(), "Output human-readable final regressor with feature names.  Computationally expensive.")
    ("save_resume", "save extra state so learning can be resumed later with new data")
    ("save_per_pass", "Save the model after every pass over data")
    ("output_feature_regularizer_binary", po::value< string >(&(all.per_feature_regularizer_output)), "Per feature regularization output file")
    ("output_feature_regularizer_text", po::value< string >(&(all.per_feature_regularizer_text)), "Per feature regularization output file, in text");  
  add_options(all);

  po::variables_map& vm = all.vm;
  if (vm.count("final_regressor")) {
    all.final_regressor_name = vm["final_regressor"].as<string>();
    if (!all.quiet)
      cerr << "final_regressor = " << vm["final_regressor"].as<string>() << endl;
  }
  else
    all.final_regressor_name = "";

  if (vm.count("readable_model"))
    all.text_regressor_name = vm["readable_model"].as<string>();

  if (vm.count("invert_hash")){
    all.inv_hash_regressor_name = vm["invert_hash"].as<string>();
    all.hash_inv = true;
  }

  if (vm.count("save_per_pass"))
    all.save_per_pass = true;

  if (vm.count("save_resume"))
    all.save_resume = true;
}

void load_input_model(vw& all, io_buf& io_temp)
{
  // Need to see if we have to load feature mask first or second.
  // -i and -mask are from same file, load -i file first so mask can use it
  if (all.vm.count("feature_mask") && all.vm.count("initial_regressor")
      && all.vm["feature_mask"].as<string>() == all.vm["initial_regressor"].as< vector<string> >()[0]) {
    // load rest of regressor
    all.l->save_load(io_temp, true, false);
    io_temp.close_file();

    // set the mask, which will reuse -i file we just loaded
    parse_mask_regressor_args(all);
  }
  else {
    // load mask first
    parse_mask_regressor_args(all);

    // load rest of regressor
    all.l->save_load(io_temp, true, false);
    io_temp.close_file();
  }
}

LEARNER::base_learner* setup_base(vw& all)
{
  LEARNER::base_learner* ret = all.reduction_stack.pop()(all);
  if (ret == nullptr)
    return setup_base(all);
  else 
    return ret;
}

void parse_reductions(vw& all)
{
  new_options(all, "Reduction options, use [option] --help for more info");
  add_options(all);
  //Base algorithms
  all.reduction_stack.push_back(GD::setup);
  all.reduction_stack.push_back(kernel_svm_setup);
  all.reduction_stack.push_back(ftrl_setup);
  all.reduction_stack.push_back(svrg_setup);
  all.reduction_stack.push_back(sender_setup);
  all.reduction_stack.push_back(gd_mf_setup);
  all.reduction_stack.push_back(print_setup);
  all.reduction_stack.push_back(noop_setup);
  all.reduction_stack.push_back(lda_setup);
  all.reduction_stack.push_back(bfgs_setup);

  //Score Users
  all.reduction_stack.push_back(active_setup);
  all.reduction_stack.push_back(nn_setup);
  all.reduction_stack.push_back(mf_setup);
  all.reduction_stack.push_back(autolink_setup);
  all.reduction_stack.push_back(lrq_setup);
  all.reduction_stack.push_back(lrqfa_setup);
  all.reduction_stack.push_back(stagewise_poly_setup);
  all.reduction_stack.push_back(scorer_setup);

  //Reductions
  all.reduction_stack.push_back(binary_setup);
  all.reduction_stack.push_back(topk_setup);
  all.reduction_stack.push_back(oaa_setup);
  all.reduction_stack.push_back(ect_setup);
  all.reduction_stack.push_back(log_multi_setup);
  all.reduction_stack.push_back(multilabel_oaa_setup);
  all.reduction_stack.push_back(csoaa_setup);
  all.reduction_stack.push_back(csldf_setup);
  all.reduction_stack.push_back(cb_algs_setup);
  all.reduction_stack.push_back(cbify_setup);
  all.reduction_stack.push_back(Search::setup);
  all.reduction_stack.push_back(bs_setup);

  all.l = setup_base(all);
}

void add_to_args(vw& all, int argc, char* argv[])
{
  for (int i = 1; i < argc; i++)
    all.args.push_back(string(argv[i]));
}

vw& parse_args(int argc, char *argv[])
{
  vw& all = *(new vw());

  add_to_args(all, argc, argv);

  size_t random_seed = 0;
  all.program_name = argv[0];

  time(&all.init_time);

  new_options(all, "VW options")
    ("random_seed", po::value<size_t>(&random_seed), "seed random number generator")
    ("ring_size", po::value<size_t>(&(all.p->ring_size)), "size of example ring");
  add_options(all);

  new_options(all, "Update options")
    ("learning_rate,l", po::value<float>(&(all.eta)), "Set learning rate")
    ("power_t", po::value<float>(&(all.power_t)), "t power value")
    ("decay_learning_rate",    po::value<float>(&(all.eta_decay_rate)),
     "Set Decay factor for learning_rate between passes")
    ("initial_t", po::value<double>(&((all.sd->t))), "initial t value")
    ("feature_mask", po::value< string >(), "Use existing regressor to determine which parameters may be updated.  If no initial_regressor given, also used for initial weights.");
  add_options(all);

  new_options(all, "Weight options")
    ("initial_regressor,i", po::value< vector<string> >(), "Initial regressor(s)")
    ("initial_weight", po::value<float>(&(all.initial_weight)), "Set all weights to an initial value of arg.")
    ("random_weights", po::value<bool>(&(all.random_weights)), "make initial weights random")
    ("input_feature_regularizer", po::value< string >(&(all.per_feature_regularizer_input)), "Per feature regularization input file");
  add_options(all);

  new_options(all, "Parallelization options")
    ("span_server", po::value<string>(&(all.span_server)), "Location of server for setting up spanning tree")
    ("unique_id", po::value<size_t>(&(all.unique_id)),"unique id used for cluster parallel jobs")
    ("total", po::value<size_t>(&(all.total)),"total number of nodes used in cluster parallel job")
    ("node", po::value<size_t>(&(all.node)),"node number in cluster parallel job");
  add_options(all);

  po::variables_map& vm = all.vm;
  msrand48(random_seed);
  parse_diagnostics(all, argc);

  all.sd->weighted_unlabeled_examples = all.sd->t;
  all.initial_t = (float)all.sd->t;

  //Input regressor header
  io_buf io_temp;
  parse_regressor_args(all, vm, io_temp);
  
  int temp_argc = 0;
  char** temp_argv = VW::get_argv_from_string(all.file_options->str(), temp_argc);
  add_to_args(all, temp_argc, temp_argv);
  for (int i = 0; i < temp_argc; i++)
    free(temp_argv[i]);
  free(temp_argv);
  
  po::parsed_options pos = po::command_line_parser(all.args).
    style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing).
    options(all.opts).allow_unregistered().run();

  vm = po::variables_map();

  po::store(pos, vm);
  po::notify(vm);
  all.file_options->str("");

  parse_feature_tweaks(all); //feature tweaks

  parse_example_tweaks(all); //example manipulation

  parse_output_model(all);
  
  parse_output_preds(all);

  parse_reductions(all);

  if (!all.quiet)
    {
      cerr << "Num weight bits = " << all.num_bits << endl;
      cerr << "learning rate = " << all.eta << endl;
      cerr << "initial_t = " << all.sd->t << endl;
      cerr << "power_t = " << all.power_t << endl;
      if (all.numpasses > 1)
	cerr << "decay_learning_rate = " << all.eta_decay_rate << endl;
    }

  load_input_model(all, io_temp);

  parse_source(all);

  enable_sources(all, all.quiet, all.numpasses);

  // force wpp to be a power of 2 to avoid 32-bit overflow
  uint32_t i = 0;
  size_t params_per_problem = all.l->increment;
  while (params_per_problem > (uint32_t)(1 << i))
    i++;
  all.wpp = (1 << i) >> all.reg.stride_shift;

  if (vm.count("help")) {
    /* upon direct query for help -- spit it out to stdout */
    cout << "\n" << all.opts << "\n";
    exit(0);
  }

  return all;
}

namespace VW {
  void cmd_string_replace_value( std::stringstream*& ss, string flag_to_replace, string new_value )
  {
    flag_to_replace.append(" "); //add a space to make sure we obtain the right flag in case 2 flags start with the same set of characters
    string cmd = ss->str();
    size_t pos = cmd.find(flag_to_replace);
    if( pos == string::npos )
      //flag currently not present in command string, so just append it to command string
      *ss << " " << flag_to_replace << new_value;
    else {
      //flag is present, need to replace old value with new value

      //compute position after flag_to_replace
      pos += flag_to_replace.size();

      //now pos is position where value starts
      //find position of next space
      size_t pos_after_value = cmd.find(" ",pos);
      if(pos_after_value == string::npos) 
        //we reach the end of the string, so replace the all characters after pos by new_value
        cmd.replace(pos,cmd.size()-pos,new_value);
      else 
        //replace characters between pos and pos_after_value by new_value
        cmd.replace(pos,pos_after_value-pos,new_value);
      ss->str(cmd);
    }
  }

  char** get_argv_from_string(string s, int& argc)
  {
    char* c = calloc_or_die<char>(s.length()+3);
    c[0] = 'b';
    c[1] = ' ';
    strcpy(c+2, s.c_str());
    substring ss = {c, c+s.length()+2};
    v_array<substring> foo = v_init<substring>();
    tokenize(' ', ss, foo);

    char** argv = calloc_or_die<char*>(foo.size());
    for (size_t i = 0; i < foo.size(); i++)
      {
	*(foo[i].end) = '\0';
	argv[i] = calloc_or_die<char>(foo[i].end-foo[i].begin+1);
        sprintf(argv[i],"%s",foo[i].begin);
      }

    argc = (int)foo.size();
    free(c);
    foo.delete_v();
    return argv;
  }

  vw* initialize(string s)
  {
    int argc = 0;
    s += " --no_stdin";
    char** argv = get_argv_from_string(s,argc);

    vw& all = parse_args(argc, argv);

    initialize_parser_datastructures(all);
    
    for(int i = 0; i < argc; i++)
      free(argv[i]);
    free(argv);

    return &all;
  }

  void delete_dictionary_entry(substring ss, v_array<feature>*A) {
    free(ss.begin);
    A->delete_v();
    delete A;
  }
  
  void finish(vw& all, bool delete_all)
  {
    if (!all.quiet)
        {
        cerr.precision(6);
        cerr << endl << "finished run";
        if(all.current_pass == 0)
            cerr << endl << "number of examples = " << all.sd->example_number;
        else{
            cerr << endl << "number of examples per pass = " << all.sd->example_number / all.current_pass;
            cerr << endl << "passes used = " << all.current_pass;
        }
        cerr << endl << "weighted example sum = " << all.sd->weighted_examples;
        cerr << endl << "weighted label sum = " << all.sd->weighted_labels;
        if(all.holdout_set_off || (all.sd->holdout_best_loss == FLT_MAX))
	  cerr << endl << "average loss = " << all.sd->sum_loss / all.sd->weighted_examples;
	else
	  cerr << endl << "average loss = " << all.sd->holdout_best_loss << " h";

        float best_constant; float best_constant_loss;
        if (get_best_constant(all, best_constant, best_constant_loss))
	  {
            cerr << endl << "best constant = " << best_constant;
            if (best_constant_loss != FLT_MIN)
	      cerr << endl << "best constant's loss = " << best_constant_loss;
	  }
	
        cerr << endl << "total feature number = " << all.sd->total_features;
        if (all.sd->queries > 0)
	  cerr << endl << "total queries = " << all.sd->queries << endl;
        cerr << endl;
        }
    
    finalize_regressor(all, all.final_regressor_name);
    all.l->finish();
    free_it(all.l);
    if (all.reg.weight_vector != nullptr)
      free(all.reg.weight_vector);
    free_parser(all);
    finalize_source(all.p);
    all.p->parse_name.erase();
    all.p->parse_name.delete_v();
    free(all.p);
    free(all.sd);
    all.reduction_stack.delete_v();
    delete all.file_options;
    for (size_t i = 0; i < all.final_prediction_sink.size(); i++)
      if (all.final_prediction_sink[i] != 1)
	io_buf::close_file_or_socket(all.final_prediction_sink[i]);
    all.final_prediction_sink.delete_v();
    for (size_t i=0; i<all.read_dictionaries.size(); i++) {
      free(all.read_dictionaries[i].name);
      all.read_dictionaries[i].dict->iter(delete_dictionary_entry);
      all.read_dictionaries[i].dict->delete_v();
      delete all.read_dictionaries[i].dict;
    }
    delete all.loss;
    if (delete_all) delete &all;
  }
}
