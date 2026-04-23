// secure_copy.cpp
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
#include <ctime>
#include <algorithm>  // [НОВОЕ] для estimate_parallel_time

constexpr size_t BUFFER_SIZE = 4096;
constexpr int WORKERS_COUNT = 4;  // [ИЗМЕНЕНО] было NUM_THREADS=3, теперь 4 по требованию
constexpr int TIMEOUT_SEC = 5;

volatile sig_atomic_t keep_running = 1;

// [НОВОЕ] Режимы работы
enum class Mode { SEQUENTIAL, PARALLEL, AUTO };

typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

struct FileJob {
    std::string input_path;
    std::string output_path;
    uint8_t key;
    set_key_fn set_key;
    caesar_fn caesar;
};

// [НОВОЕ] Структуры для статистики
struct FileStats {
    std::string filename;
    double duration;  // секунды
};

struct StatsCollector {
    std::vector<FileStats> file_stats;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  // защита для многопоточного доступа
    struct timespec total_start{};
    struct timespec total_end{};
    bool was_sequential_mode = false;  // какой режим фактически выполнялся
    
    void start_total() {
        clock_gettime(CLOCK_MONOTONIC, &total_start);  // [ИЗМЕНЕНО] CLOCK_MONOTONIC вместо REALTIME
    }
    
    void end_total() {
        clock_gettime(CLOCK_MONOTONIC, &total_end);
    }
    
    double get_total_time() const {
        return (total_end.tv_sec - total_start.tv_sec) + 
               (total_end.tv_nsec - total_start.tv_nsec) / 1e9;
    }
    
    double get_average_time() const {
        if (file_stats.empty()) return 0;
        double sum = 0;
        for (const auto& fs : file_stats) sum += fs.duration;
        return sum / file_stats.size();
    }
    
    // [НОВОЕ] Оценка времени для параллельного режима с заданным числом воркеров
    double estimate_parallel_time(int num_workers) const {
        if (file_stats.empty()) return 0;
        std::vector<double> worker_load(num_workers, 0.0);
        for (const auto& fs : file_stats) {
            int min_idx = 0;
            for (int w = 1; w < num_workers; ++w)
                if (worker_load[w] < worker_load[min_idx]) min_idx = w;
            worker_load[min_idx] += fs.duration;
        }
        return *std::max_element(worker_load.begin(), worker_load.end());
    }
    
    void print_stats(const std::string& mode_name) const {
        std::cout << "\n=== Statistics: " << mode_name << " ===" << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(3) 
                  << get_total_time() << " s" << std::endl;
        std::cout << "Average time per file: " << get_average_time() << " s" << std::endl;
        std::cout << "Files processed: " << file_stats.size() << std::endl;
        std::cout << "================================\n" << std::endl;
    }
    
    ~StatsCollector() {
        pthread_mutex_destroy(&mutex);
    }
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
    StatsCollector* stats;  // [НОВОЕ] для сбора статистики в параллельном режиме
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
            << "Thread-" << thread_id << " "
            << status << ": " << filename << std::endl;
    }
    pthread_mutex_unlock(&logger->mutex);
}

// [НОВОЕ] Вынесенная логика обработки одного файла - используется в обоих режимах
bool process_single_file(const FileJob& job, double* out_duration) {
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);  // [ИЗМЕНЕНО] точный замер
    
    struct stat st;
    if (stat(job.input_path.c_str(), &st) != 0) {
        std::cerr << "File not found: " << job.input_path << std::endl;
        return false;
    }
    
    std::ifstream in(job.input_path, std::ios::binary);
    if (!in) { std::cerr << "Error opening: " << job.input_path << std::endl; return false; }
    
    std::ofstream out(job.output_path, std::ios::binary);
    if (!out) { std::cerr << "Error creating: " << job.output_path << std::endl; return false; }
    
    job.set_key(static_cast<char>(job.key));
    
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    bool success = true;
    while (keep_running && in) {
        in.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        job.caesar(buffer.data(), buffer.data(), static_cast<int>(n));
        out.write(reinterpret_cast<const char*>(buffer.data()), n);
        if (!out) { success = false; break; }
    }
    
    in.close(); out.close();
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    
    if (out_duration) {
        *out_duration = (ts_end.tv_sec - ts_start.tv_sec) + 
                       (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    }
    return success && keep_running;
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
        
        double duration = 0;
        bool success = process_single_file(job, &duration);
        
        // [НОВОЕ] Запись статистики (с защитой мьютексом)
        if (td->stats && duration > 0) {
            pthread_mutex_lock(&td->stats->mutex);
            td->stats->file_stats.push_back({job.input_path, duration});
            pthread_mutex_unlock(&td->stats->mutex);
        }
        
        // Логирование (оставлено без изменений)
        if (success && keep_running) {
            std::cout << "Processed: " << job.input_path << " -> " << job.output_path << std::endl;
            write_log(td->logger, td->thread_id, job.input_path, "SUCCESS");
        } else if (!keep_running) {
            write_log(td->logger, td->thread_id, job.input_path, "INTERRUPTED");
        } else {
            write_log(td->logger, td->thread_id, job.input_path, "ERROR");
        }
    }
    return nullptr;
}

bool ensure_dir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0755) == 0;
}

std::string get_basename(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// [НОВОЕ] Парсинг аргументов с поддержкой --mode=xxx
struct ParsedArgs {
    std::vector<std::string> files;
    std::string out_dir;
    uint8_t key;
    Mode mode = Mode::AUTO;
};

ParsedArgs parse_args(int argc, char* argv[]) {
    ParsedArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--mode=", 0) == 0) {
            std::string mode_str = arg.substr(7);
            if (mode_str == "sequential") args.mode = Mode::SEQUENTIAL;
            else if (mode_str == "parallel") args.mode = Mode::PARALLEL;
            else if (mode_str == "auto") args.mode = Mode::AUTO;
        } else if (i == argc - 2) {
            args.out_dir = arg;
        } else if (i == argc - 1) {
            args.key = static_cast<uint8_t>(std::atoi(arg.c_str()) & 0xFF);
        } else {
            args.files.push_back(arg);
        }
    }
    // [НОВОЕ] Авто-выбор режима по эвристике задания
    if (args.mode == Mode::AUTO) {
        args.mode = (args.files.size() < 5) ? Mode::SEQUENTIAL : Mode::PARALLEL;
    }
    return args;
}

int main(int argc, char* argv[]) {
    ParsedArgs args = parse_args(argc, argv);
    
    if (args.files.empty() || args.out_dir.empty()) {
        std::cerr << "Usage: ./secure_copy [--mode=sequential|parallel|auto] <files...> <out_dir> <key>\n";
        return 1;
    }
    
    if (!ensure_dir(args.out_dir)) {
        std::cerr << "Cannot create output directory: " << args.out_dir << std::endl;
        return 1;
    }
    
    void* handle = dlopen("./libcaesar.so", RTLD_NOW);
    if (!handle) { std::cerr << "Library error: " << dlerror() << std::endl; return 1; }
    
    auto set_key = reinterpret_cast<set_key_fn>(dlsym(handle, "set_key"));
    auto caesar = reinterpret_cast<caesar_fn>(dlsym(handle, "caesar"));
    if (!set_key || !caesar) { std::cerr << "Symbol error: " << dlerror() << std::endl; dlclose(handle); return 1; }
    
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    
    JobQueue queue;
    Logger logger{"log.txt"};
    std::ofstream clear_log(logger.log_path, std::ios::trunc);
    clear_log.close();
    
    StatsCollector stats;
    
    // ========== ПОСЛЕДОВАТЕЛЬНЫЙ РЕЖИМ ==========
    if (args.mode == Mode::SEQUENTIAL) {
        stats.was_sequential_mode = true;
        stats.start_total();
        
        for (const auto& filepath : args.files) {
            FileJob job{filepath, args.out_dir + "/" + get_basename(filepath), 
                       args.key, set_key, caesar};
            
            double duration = 0;
            bool success = process_single_file(job, &duration);
            
            if (duration > 0) {
                pthread_mutex_lock(&stats.mutex);
                stats.file_stats.push_back({filepath, duration});
                pthread_mutex_unlock(&stats.mutex);
            }
            
            if (success && keep_running) {
                std::cout << "Processed: " << job.input_path << " -> " << job.output_path << std::endl;
                write_log(&logger, 0, job.input_path, "SUCCESS");
            } else if (!keep_running) {
                write_log(&logger, 0, job.input_path, "INTERRUPTED");
            } else {
                write_log(&logger, 0, job.input_path, "ERROR");
            }
        }
        stats.end_total();
        stats.print_stats("SEQUENTIAL");
    }
    // ========== ПАРАЛЛЕЛЬНЫЙ РЕЖИМ ==========
    else if (args.mode == Mode::PARALLEL) {
        stats.was_sequential_mode = false;
        stats.start_total();
        
        // Заполнение очереди
        for (const auto& filepath : args.files) {
            FileJob job{filepath, args.out_dir + "/" + get_basename(filepath), 
                       args.key, set_key, caesar};
            pthread_mutex_lock(&queue.mutex);
            queue.jobs.push(job);
            pthread_mutex_unlock(&queue.mutex);
        }
        
        pthread_mutex_lock(&queue.mutex);
        queue.done = true;
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.mutex);
        
        ThreadData td[WORKERS_COUNT];
        pthread_t threads[WORKERS_COUNT];
        
        for (int i = 0; i < WORKERS_COUNT; ++i) {
            td[i] = {&queue, &logger, i, &stats};  // [НОВОЕ] передача stats
            if (pthread_create(&threads[i], nullptr, worker_thread, &td[i]) != 0) {
                std::cerr << "Failed to create thread " << i << std::endl;
                keep_running = 0;
            }
        }
        
        for (int i = 0; i < WORKERS_COUNT; ++i) pthread_join(threads[i], nullptr);
        stats.end_total();
        stats.print_stats("PARALLEL");
    }
    
    // [НОВОЕ] Сравнительная таблица для AUTO режима
    if (args.mode == Mode::AUTO) {
        double actual_time = stats.get_total_time();
        double sequential_est = 0;
        for (const auto& fs : stats.file_stats) sequential_est += fs.duration;
        double parallel_est = stats.estimate_parallel_time(WORKERS_COUNT);
        
        std::cout << "\n=== Mode Comparison (AUTO) ===" << std::endl;
        std::cout << "Selected mode: " << (stats.was_sequential_mode ? "SEQUENTIAL" : "PARALLEL") << std::endl;
        std::cout << "Actual time: " << std::fixed << std::setprecision(3) << actual_time << " s" << std::endl;
        std::cout << "Alternative estimate: " 
                  << (stats.was_sequential_mode ? "PARALLEL" : "SEQUENTIAL") << ": "
                  << (stats.was_sequential_mode ? parallel_est : sequential_est) << " s" << std::endl;
        if (actual_time > 0.001 && (stats.was_sequential_mode ? parallel_est : sequential_est) > 0.001) {
            double speedup = (stats.was_sequential_mode ? actual_time / parallel_est : sequential_est / actual_time);
            std::cout << "Estimated speedup of selected mode: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
        std::cout << "==============================\n" << std::endl;
    }
    
    // Очистка ресурсов
    pthread_mutex_destroy(&queue.mutex);
    pthread_cond_destroy(&queue.cond);
    pthread_mutex_destroy(&logger.mutex);
    dlclose(handle);
    
    if (!keep_running) {
        std::cout << "Операция прервана пользователем" << std::endl;
        write_log(&logger, -1, "-", "PROGRAM_INTERRUPTED");
        return 130;
    }
    
    std::cout << "Готово. Обработано файлов: " << args.files.size() << std::endl;
    std::cout << "Лог сохранён в: log.txt" << std::endl;
    return 0;
}