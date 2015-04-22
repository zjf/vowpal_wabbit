/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD
license as described in the file LICENSE.
 */
#ifdef _WIN32
#include <WinSock2.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif
#include <sys/timeb.h>
#include "parse_args.h"
#include "accumulate.h"
#include "best_constant.h"

using namespace std;

int main(int argc, char *argv[])
{
  try {
    vw& all = parse_args(argc, argv);
    all.vw_is_main = true;
    struct timeb t_start, t_end;
    ftime(&t_start);
    
    if (!all.quiet && !all.bfgs && !all.searchstr)
        {
        	std::cerr << std::left
        	          << std::setw(shared_data::col_avg_loss) << std::left << "average"
        		  << " "
        		  << std::setw(shared_data::col_since_last) << std::left << "since"
        		  << " "
			  << std::right
        		  << std::setw(shared_data::col_example_counter) << "example"
        		  << " "
        		  << std::setw(shared_data::col_example_weight) << "example"
        		  << " "
        		  << std::setw(shared_data::col_current_label) << "current"
        		  << " "
        		  << std::setw(shared_data::col_current_predict) << "current"
        		  << " "
        		  << std::setw(shared_data::col_current_features) << "current"
        		  << std::endl;
        	std::cerr << std::left
        	          << std::setw(shared_data::col_avg_loss) << std::left << "loss"
        		  << " "
        		  << std::setw(shared_data::col_since_last) << std::left << "last"
        		  << " "
			  << std::right
        		  << std::setw(shared_data::col_example_counter) << "counter"
        		  << " "
        		  << std::setw(shared_data::col_example_weight) << "weight"
        		  << " "
        		  << std::setw(shared_data::col_current_label) << "label"
        		  << " "
        		  << std::setw(shared_data::col_current_predict) << "predict"
        		  << " "
        		  << std::setw(shared_data::col_current_features) << "features"
        		  << std::endl;
        }

    VW::start_parser(all);
    LEARNER::generic_driver(all);
    VW::end_parser(all);

    ftime(&t_end);
    double net_time = (int) (1000.0 * (t_end.time - t_start.time) + (t_end.millitm - t_start.millitm)); 
    if(!all.quiet && all.span_server != "")
        cerr<<"Net time taken by process = "<<net_time/(double)(1000)<<" seconds\n";

    if(all.span_server != "") {
        float loss = (float)all.sd->sum_loss;
        all.sd->sum_loss = (double)accumulate_scalar(all, all.span_server, loss);
        float weighted_examples = (float)all.sd->weighted_examples;
        all.sd->weighted_examples = (double)accumulate_scalar(all, all.span_server, weighted_examples);
        float weighted_labels = (float)all.sd->weighted_labels;
        all.sd->weighted_labels = (double)accumulate_scalar(all, all.span_server, weighted_labels);
        float weighted_unlabeled_examples = (float)all.sd->weighted_unlabeled_examples;
        all.sd->weighted_unlabeled_examples = (double)accumulate_scalar(all, all.span_server, weighted_unlabeled_examples);
        float example_number = (float)all.sd->example_number;
        all.sd->example_number = (uint64_t)accumulate_scalar(all, all.span_server, example_number);
        float total_features = (float)all.sd->total_features;
        all.sd->total_features = (uint64_t)accumulate_scalar(all, all.span_server, total_features);
    }

    VW::finish(all);
  } catch (exception& e) {
    // vw is implemented as a library, so we use 'throw exception()'
    // error 'handling' everywhere.  To reduce stderr pollution
    // everything gets caught here & the error message is printed
    // sans the excess exception noise, and core dump.
    cerr << "vw: " << e.what() << endl;
    exit(1);
  }
  return 0;
}

