#include <iostream>
#include <fstream>
#include <vector>
#include <boost/locale.hpp>
#include <boost/filesystem.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>
#include <future>
#include <cmath>
#include <deque>
#include "tbb/tbb.h"

namespace bl = boost::locale;
namespace bfs = boost::filesystem;

typedef std::unordered_map<std::string, int> um_t;
typedef std::pair<std::string, int> p_t;
typedef std::vector<std::string> v_t;

inline std::chrono::steady_clock::time_point get_current_time_fenced() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto res_time = std::chrono::steady_clock::now();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return res_time;
}

template<class D>
inline long long to_us(const D& d)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

auto parse_config(std::ifstream &config){
    std::string line;
    std::getline(config, line);

    std::istringstream line_s(line);
    std::string key;

    if( std::getline(line_s, key, '=') ){
        std::string value;
        if( std::getline(line_s, value) ) {
            value.erase(remove(value.begin(), value.end(), '\"'), value.end());
            return value;
        }
    }
}

template<class element>
class thsafe_q {
private:
    std::deque<element> que_m;
    mutable std::mutex mtx;
    std::condition_variable cv_m;
    std::condition_variable cv_full_m;
    size_t max_size_m = 10000;
    std::atomic<size_t> producers_m{};
public:
    thsafe_q() = default;

    void push(const element& el) {
//        std::lock_guard<std::mutex> lg{mtx};
        std::unique_lock<std::mutex> ul{mtx};
        cv_full_m.wait(ul, [this]() { return que_m.size() < max_size_m; });
        que_m.push_back(el);
        cv_m.notify_one();
    }

    element pop() {
        std::unique_lock<std::mutex> ul{mtx};
        cv_m.wait(ul, [this]() { return !que_m.empty(); });
        element el = que_m.front();
        que_m.pop_front();
        return el;
    }

    std::pair<element, element> pop_two() {
        element el1, el2;
        std::unique_lock<std::mutex> ul{mtx};
//        std::cout << "Waiting for 1 more element" << std::endl;
        cv_m.wait(ul, [this]() { return que_m.size() > 1; });
        el1 = que_m.front();
        que_m.pop_front();
        el2 = que_m.front();
        que_m.pop_front();
        return std::make_pair(el1, el2);
    }

    size_t get_size() const {
        std::lock_guard<std::mutex> lg{mtx};
        return que_m.size();
    }

    void add_producers() {
        producers_m++;
    }

    void remove_producers() {
        producers_m--;
    }

    size_t producers(){
        return producers_m;
    }
};

void merge(thsafe_q<um_t>& que_m, thsafe_q<std::string>& raws_que_m) {
    while (true) {
//        std::cout << "Merge start, dict size: " << que_m.get_size() << std::endl;
        um_t map1, map2;
        std::pair<um_t, um_t> map_pair = que_m.pop_two();
        map1 = map_pair.first;
        map2 = map_pair.second;
        if (map1.empty() || map2.empty()) {
            auto que_size = que_m.get_size();
            if (map1.empty()) {
//                std::cout << "if 1" << std::endl;
                que_m.push(map2);
                que_m.push(map1);
            } else if (map2.empty()) {
//                std::cout << "if 2" << std::endl;
                que_m.push(map1);
                que_m.push(map2);
            }
            if (que_size < 1) {
                std::cout << "if end" << std::endl;
                break;
            }
        } else {
//            std::cout << "else" << std::endl;
            for (auto &el : map2) {
                map1[el.first] += el.second;
            }
            que_m.push(map1);
        }
    }
}

void filenames_enqueue(const bfs::path& p, thsafe_q<bfs::path>& filenames_que_m) {
    try {
        if (exists(p)) {
            if (is_regular_file(p)) {
                if (file_size(p) < 1000000) {
                    filenames_que_m.push(p);
//                    std::cout << "Que size: " << filenames_que_m.get_size() << std::endl;
                }
            } else if (is_directory(p)) {
                for (bfs::directory_entry &x : bfs::directory_iterator(p)) {
                    filenames_enqueue(x.path(), std::ref(filenames_que_m));
                }
            } else
                std::cout << p << " exists, but is not a regular file or directory\n";
        }
        else
            std::cout << p << " does not exist\n";
    }

    catch (const bfs::filesystem_error &ex) {
        std::cout << ex.what() << '\n';
    }
}

void read_filenames(thsafe_q<bfs::path>& filenames_que_m, thsafe_q<std::string>& raws_que_m){
//    std::cout << "Read filenames start" << std::endl;
    while(true) {
//        std::cout << "Raws queue size: " << raws_que_m.get_size() << std::endl;
        auto p = filenames_que_m.pop();
        if(p.empty()){
            raws_que_m.push("");
            break;
        }
        std::ifstream raw_file(p.string(), std::ios::binary);
        auto buffer = static_cast<std::ostringstream &>(std::ostringstream{} << raw_file.rdbuf()).str();
        raws_que_m.push(std::move(buffer));
        std::cout << "Pushing binary, size: " << raws_que_m.get_size() << std::endl;
    }
}

void parse_raw(thsafe_q<std::string>& raws_que_m, thsafe_q<um_t>& dict_que_m){
    raws_que_m.add_producers();
//    std::cout << "Producers: " << raws_que_m.producers() << std::endl;
    while(true) {
//        std::cout << "Producers: " << raws_que_m.producers() << std::endl;
        std::vector<p_t> vec_of_pairs;
        v_t words;
        auto buffer = raws_que_m.pop();
        if(buffer.empty()){
//            std::cout << "push empty" << std::endl;
            raws_que_m.push("");
            break;
        }
        const char *buf_ptr = buffer.c_str();

        struct archive *a;
        struct archive_entry *entry;
        int r;

        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);

        r = archive_read_open_memory(a, buf_ptr, buffer.size());

        //Extract files from archive and paste content to buffer
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            la_int64_t entry_size = archive_entry_size(entry);
            std::string output(entry_size, char{});
            r = archive_read_data(a, &output[0], output.size());
            buffer = output;
        }

        //No segmentation, convert whole buffer
        buffer = bl::normalize(buffer, bl::norm_default);
        buffer = bl::fold_case(buffer);

        //No segmentation, traverse through the whole buffer
        bl::boundary::ssegment_index map(bl::boundary::word, buffer.begin(), buffer.end());
        map.rule(boost::locale::boundary::word_any);

        for (bl::boundary::ssegment_index::iterator it = map.begin(), e = map.end(); it != e; ++it) {
            words.emplace_back(*it);
        }

        um_t mapped_words;
        for (auto& word : words) {
            mapped_words[word]++;
        }

        dict_que_m.push(std::move(mapped_words));
//        std::cout << "Pushing dict, que size: " << dict_que_m.get_size() << std::endl;
    }
    raws_que_m.remove_producers();
    if(raws_que_m.producers() == 0){
        std::cout << "FINALLY" << std::endl;
        dict_que_m.push(um_t {});
    }
}

void mt_parse_raws(thsafe_q<bfs::path>& filenames_que_m, const bfs::path& path, thsafe_q<std::string>& raws_que_m, thsafe_q<um_t>& dict_que_m, const int parse_threads, const int merge_threads ){
//    read_filenames(std::ref(filenames_que_m), std::ref(raws_que_m));
//    filenames_enqueue("./data/" + infile, std::ref(filenames_que_m));
    std::vector<std::thread> th_vec;
    th_vec.reserve(parse_threads + merge_threads + 2);
    std::cout << "parse" << std::endl;

    th_vec.emplace_back(filenames_enqueue, path, std::ref(filenames_que_m));
    th_vec.emplace_back(read_filenames, std::ref(filenames_que_m), std::ref(raws_que_m));
//    raws_que_m.push("");
    for (int i = 0; i < parse_threads; i++) {
        th_vec.emplace_back(parse_raw, std::ref(raws_que_m), std::ref(dict_que_m));
    }
    for (int i = 0; i < merge_threads; i++) {
        th_vec.emplace_back(merge, std::ref(dict_que_m), std::ref(raws_que_m));
    }

    th_vec[0].join();
    filenames_que_m.push("");
//    std::cout << "Filenames queue size: " << filenames_que_m.get_size() << std::endl;
//    std::cout << "Raw files queue size: " << raws_que_m.get_size() << std::endl;
    std::cout << "Joining all threads..." << std::endl;
    for(int i = 1; i < parse_threads + merge_threads + 2; i++) {
        th_vec[i].join();
    }
    std::cout << "Threads joined" << std::endl;

    th_vec.clear();

    std::cout << "done!" << std::endl;
}

int main(int argc, char* argv[]){
    namespace bl = boost::locale;
    bl::localization_backend_manager::global().select("icu");
    bl::generator gen;
    std::locale::global(gen("en_US.UTF-8"));

    std::string config_path;
    if(argc == 1){
        config_path = "config.dat";
    }
    else if(argc == 2){
        config_path = argv[1];
    }
    else {
        throw std::runtime_error("Too many arguments");
    }

    //Read from configuration file
    std::ifstream config("./config/" + config_path);
    std::string infile, out_by_a, out_by_n, th1, th2;

    //Parse the configuration file
    infile = parse_config(config);
    out_by_a = parse_config(config);
    out_by_n = parse_config(config);
    th1 = parse_config(config);
    th2 = parse_config(config);
    int parse_threads = std::stoi(th1);
    int merge_threads = std::stoi(th2);

    //Queues for file paths and raw files
    thsafe_q<bfs::path> filenames_que_m;
    thsafe_q<std::string> raws_que_m;
    thsafe_q<um_t> dict_que_m;

    //Set writing path
    std::ofstream by_name("./output/" + out_by_a, std::ios::binary);
    std::ofstream by_num("./output/" + out_by_n, std::ios::binary);

    auto total_begin = get_current_time_fenced();

    auto filenames_begin = get_current_time_fenced();
    std::cout << "Filenames enqueue..." << std::endl;
    //FILENAMES_ENQUEUE
    //    filenames_enqueue("./data/" + infile, std::ref(filenames_que_m));
    auto filenames_time = get_current_time_fenced() - filenames_begin;

    auto raws_begin = get_current_time_fenced();


    std::cout << "Reading filenames..." << std::endl;
    auto raws_time = get_current_time_fenced() - raws_begin;
    //READ_FILENAMES
    auto parse_begin = get_current_time_fenced();
    std::cout << "Parsing raws..." << std::endl;
    mt_parse_raws(std::ref(filenames_que_m), "./data/" + infile, std::ref(raws_que_m), std::ref(dict_que_m), parse_threads, merge_threads);
    auto parse_time = get_current_time_fenced() - parse_begin;

    auto total_time = get_current_time_fenced() - total_begin;

//    std::cout << "Parsing: " << to_us(parse_time) << std::endl;
//    std::cout << "Number of maps: " << dict_que_m.get_size() << std::endl;

    std::cout << "Total: " << to_us(total_time) << std::endl;

    std::cout << "Number of maps in queue: " << dict_que_m.get_size() << std::endl;

    std::vector<p_t> vec_of_pairs;

//    if(dict_que_m.get_size() > 2){
//        throw std::runtime_error("Too many maps in queue");
//    }

//    for(int i = 0; i < dict_que_m.get_size(); i++){
//        um_t map_item = dict_que_m.pop();
//        std::cout << map_item.size() << std::endl;
//    }

    for (auto &kv: dict_que_m.pop()) {
        vec_of_pairs.emplace_back(std::move(kv));
    }

    //Sort vector of pairs by name and write to file
    std::sort(std::begin(vec_of_pairs), std::end(vec_of_pairs),
            [](auto &left, auto &right) {return left.first < right.first;});

    for(auto &word_pair : vec_of_pairs){
        by_name << word_pair.first << "\t:\t" << word_pair.second << std::endl;
    }

    //Sort vector of pairs by number and write to file
    std::sort(std::begin(vec_of_pairs), std::end(vec_of_pairs),
              [](auto &left, auto &right) {return left.second > right.second;});

    for(auto &word_pair : vec_of_pairs){
        by_num << word_pair.first << "\t:\t" << word_pair.second << std::endl;
    }
}
