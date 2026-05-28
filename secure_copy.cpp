#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <cstdlib>
#include <pthread.h>
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include <queue>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

constexpr int WORKERS_COUNT = 5;
constexpr int TIMEOUT_SEC = 5;

volatile sig_atomic_t keep_running = 1;

enum class Mode { ADD, LIST, GET, NONE };

typedef void* (*rc4_init_fn)(const uint8_t*, int);
typedef void (*rc4_crypt_fn)(void*, void*, void*, int);
typedef void (*rc4_free_fn)(void*);

struct FileJob {
    std::string input_path;
    std::string relative_name;
};

struct JobQueue {
    std::queue<FileJob> jobs;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool done = false;
};

struct Logger {
    std::string log_path;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
};

struct ThreadData {
    JobQueue* queue;
    Logger* logger;
    int thread_id;
    std::string master_key;
    std::string image_path;
    pthread_mutex_t* image_mutex;
    rc4_init_fn init_fn;
    rc4_crypt_fn crypt_fn;
    rc4_free_fn free_fn;
};

void sigint_handler(int) { keep_running = 0; }

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void write_log(Logger* logger, int thread_id, const std::string& filename, const std::string& status) {
    pthread_mutex_lock(&logger->mutex);
    std::ofstream log(logger->log_path, std::ios::app);
    if (log) {
        log << "[" << get_timestamp() << "] "
            << "Thread-" << thread_id << " " << status << ": " << filename << std::endl;
    }
    pthread_mutex_unlock(&logger->mutex);
}

void* worker_thread(void* arg) {
    ThreadData* td = static_cast<ThreadData*>(arg);
    
    while (keep_running) {
        FileJob job;
        bool has_job = false;
        
        pthread_mutex_lock(&td->queue->mutex);
        while (td->queue->jobs.empty() && !td->queue->done && keep_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += TIMEOUT_SEC;
            int rc = pthread_cond_timedwait(&td->queue->cond, &td->queue->mutex, &ts);
            if (rc == ETIMEDOUT && (td->queue->done || !keep_running)) {
                pthread_mutex_unlock(&td->queue->mutex);
                return nullptr;
            }
        }
        if (td->queue->jobs.empty() && td->queue->done) {
            pthread_mutex_unlock(&td->queue->mutex);
            break;
        }
        if (!td->queue->jobs.empty()) {
            job = td->queue->jobs.front();
            td->queue->jobs.pop();
            has_job = true;
        }
        pthread_mutex_unlock(&td->queue->mutex);
        
        if (!has_job) break;
        
        std::ifstream in(job.input_path, std::ios::binary);
        if (!in) { 
            write_log(td->logger, td->thread_id, job.input_path, "ERROR_OPEN");
            continue; 
        }
        
        in.seekg(0, std::ios::end);
        uint32_t filesize = in.tellg();
        in.seekg(0, std::ios::beg);
        
        std::vector<uint8_t> data(filesize);
        if (filesize > 0) in.read(reinterpret_cast<char*>(data.data()), filesize);
        in.close();
        
        uint8_t salt[16];
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (urandom) {
            urandom.read(reinterpret_cast<char*>(salt), 16);
            urandom.close();
        } else {
            for(int i = 0; i < 16; ++i) salt[i] = rand() % 256;
        }
        
        std::vector<uint8_t> key_salt(td->master_key.begin(), td->master_key.end());
        key_salt.insert(key_salt.end(), salt, salt + 16);
        
        void* ctx = td->init_fn(key_salt.data(), key_salt.size());
        td->crypt_fn(ctx, data.data(), data.data(), filesize);
        td->free_fn(ctx);
        
        uint32_t namelen = job.relative_name.size();
        
        // Эксклюзивная запись в файл-образ
        pthread_mutex_lock(td->image_mutex);
        std::ofstream out(td->image_path, std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char*>(&filesize), 4);
        out.write(reinterpret_cast<const char*>(&namelen), 4);
        out.write(reinterpret_cast<const char*>(salt), 16);
        out.write(job.relative_name.c_str(), namelen);
        if (filesize > 0) out.write(reinterpret_cast<const char*>(data.data()), filesize);
        out.close();
        pthread_mutex_unlock(td->image_mutex);
        
        std::cout << "Added: " << job.relative_name << std::endl;
        write_log(td->logger, td->thread_id, job.input_path, "SUCCESS");
    }
    return nullptr;
}

struct ParsedArgs {
    Mode mode = Mode::NONE;
    std::string image_path;
    std::string key;
    std::string out_file;
    std::string get_target;
    std::vector<std::string> add_targets;
};

ParsedArgs parse_args(int argc, char* argv[]) {
    ParsedArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-add") args.mode = Mode::ADD;
        else if (arg == "-list") args.mode = Mode::LIST;
        else if (arg == "-get") args.mode = Mode::GET;
        else if (arg == "-key" && i + 1 < argc) args.key = argv[++i];
        else if (arg == "-image" && i + 1 < argc) args.image_path = argv[++i];
        else if (arg == "-out" && i + 1 < argc) args.out_file = argv[++i];
        else {
            if (args.mode == Mode::ADD) args.add_targets.push_back(arg);
            else if (args.mode == Mode::GET) args.get_target = arg;
        }
    }
    return args;
}

void list_image(const std::string& image_path) {
    std::ifstream in(image_path, std::ios::binary);
    if (!in) { std::cerr << "Image not found or cannot be opened.\n"; return; }
    
    struct FileInfo { std::string name; uint32_t size; };
    std::vector<FileInfo> files;
    
    while (in.peek() != EOF) {
        uint32_t size, namelen;
        if (!in.read(reinterpret_cast<char*>(&size), 4)) break;
        in.read(reinterpret_cast<char*>(&namelen), 4);
        in.seekg(16, std::ios::cur); 
        
        std::string name(namelen, '\0');
        in.read(&name[0], namelen);
        files.push_back({name, size});
        in.seekg(size, std::ios::cur); 
    }
    
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b){
        return a.name < b.name;
    });
    
    std::cout << "\n=== Files in " << image_path << " ===\n";
    for (const auto& f : files) {
        std::cout << f.name << " \t [" << f.size << " bytes]\n";
    }
    std::cout << "=================================\n";
}

void get_file(const std::string& image_path, const std::string& key, const std::string& target_file, 
              const std::string& out_file, rc4_init_fn init, rc4_crypt_fn crypt, rc4_free_fn free_fn) {
    std::ifstream in(image_path, std::ios::binary);
    if (!in) { std::cerr << "Image not found.\n"; return; }
    
    while (in.peek() != EOF) {
        uint32_t size, namelen;
        if (!in.read(reinterpret_cast<char*>(&size), 4)) break;
        in.read(reinterpret_cast<char*>(&namelen), 4);
        
        uint8_t salt[16];
        in.read(reinterpret_cast<char*>(salt), 16);
        
        std::string name(namelen, '\0');
        in.read(&name[0], namelen);
        
        if (name == target_file) {
            std::vector<uint8_t> data(size);
            if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
            
            std::vector<uint8_t> key_salt(key.begin(), key.end());
            key_salt.insert(key_salt.end(), salt, salt + 16);
            
            void* ctx = init(key_salt.data(), key_salt.size());
            crypt(ctx, data.data(), data.data(), size);
            free_fn(ctx);
            
            std::ofstream out(out_file, std::ios::binary);
            if (size > 0) out.write(reinterpret_cast<char*>(data.data()), size);
            std::cout << "Successfully extracted to " << out_file << "\n";
            return;
        } else {
            in.seekg(size, std::ios::cur);
        }
    }
    std::cerr << "Error: File '" << target_file << "' not found in image.\n";
}

int main(int argc, char* argv[]) {
    ParsedArgs args = parse_args(argc, argv);
    
    if (args.mode == Mode::NONE || args.image_path.empty()) {
        std::cerr << "Usage:\n"
                  << "  ./secure_copy -add -key <key> -image <img_path> <files/dirs...>\n"
                  << "  ./secure_copy -list -image <img_path>\n"
                  << "  ./secure_copy -get -image <img_path> -key <key> -out <output_file> <file_name>\n";
        return 1;
    }

    // Сохранено старое название библиотеки для минимального diff'a
    void* handle = dlopen("./libcaesar.so", RTLD_NOW);
    if (!handle) { std::cerr << "Library error: " << dlerror() << std::endl; return 1; }
    
    auto rc4_init = reinterpret_cast<rc4_init_fn>(dlsym(handle, "rc4_init"));
    auto rc4_crypt = reinterpret_cast<rc4_crypt_fn>(dlsym(handle, "rc4_crypt"));
    auto rc4_free = reinterpret_cast<rc4_free_fn>(dlsym(handle, "rc4_free"));
    
    if (!rc4_init || !rc4_crypt || !rc4_free) { 
        std::cerr << "Symbol error: " << dlerror() << std::endl; 
        dlclose(handle); 
        return 1; 
    }

    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    if (args.mode == Mode::ADD) {
        JobQueue queue;
        Logger logger{"log.txt"};
        pthread_mutex_t image_mutex = PTHREAD_MUTEX_INITIALIZER;
        
        for (const auto& target : args.add_targets) {
            if (fs::is_directory(target)) {
                for (const auto& entry : fs::recursive_directory_iterator(target)) {
                    if (fs::is_regular_file(entry)) {
                        std::string rel_path = "/" + fs::relative(entry.path(), target).string();
                        queue.jobs.push({entry.path().string(), rel_path});
                    }
                }
            } else if (fs::is_regular_file(target)) {
                queue.jobs.push({target, "/" + fs::path(target).filename().string()});
            } else {
                std::cerr << "Warning: Cannot access " << target << "\n";
            }
        }
        
        queue.done = true;
        
        ThreadData td[WORKERS_COUNT];
        pthread_t threads[WORKERS_COUNT];
        
        for (int i = 0; i < WORKERS_COUNT; ++i) {
            td[i] = {&queue, &logger, i, args.key, args.image_path, &image_mutex, rc4_init, rc4_crypt, rc4_free};
            if (pthread_create(&threads[i], nullptr, worker_thread, &td[i]) != 0) {
                std::cerr << "Failed to create thread " << i << std::endl;
                keep_running = 0;
            }
        }
        
        for (int i = 0; i < WORKERS_COUNT; ++i) pthread_join(threads[i], nullptr);
        pthread_mutex_destroy(&image_mutex);
        pthread_mutex_destroy(&queue.mutex);
        pthread_cond_destroy(&queue.cond);
        pthread_mutex_destroy(&logger.mutex);
        
    } else if (args.mode == Mode::LIST) {
        list_image(args.image_path);
    } else if (args.mode == Mode::GET) {
        if (args.key.empty() || args.out_file.empty() || args.get_target.empty()) {
            std::cerr << "Missing arguments for -get mode.\n";
        } else {
            get_file(args.image_path, args.key, args.get_target, args.out_file, rc4_init, rc4_crypt, rc4_free);
        }
    }

    dlclose(handle);
    return 0;
}