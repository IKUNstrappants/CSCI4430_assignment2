#include <iostream>
#include <unordered_map>
#include <string>
#include <ctime>
#include <cstdlib>

using namespace std;


double calculate_throughput(double data_size, long long start_time, long long end_time){
    double time_diff_seconds = (end_time - start_time) / 1000.0;  
    return data_size / time_diff_seconds;
}

unordered_map<std::string, double> clients_throughput;

double update_throughput(const string& client_id, double new_throughput, double alpha) {

    double old_throughput = clients_throughput[client_id];

    if (old_throughput == 0) {
        old_throughput = new_throughput;
    } 
    else {
        new_throughput  = alpha * new_throughput + (1-alpha) * old_throughput;

        clients_throughput[client_id] = new_throughput;
    }
    return new_throughput;
}
