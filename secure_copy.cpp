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

constexpr size_t BUFFER_SIZE = 4096;
constexpr int NUM_THREADS = 3;
constexpr int TIMEOUT_SEC = 5;

volatile sig_atomic_t keep_running = 1;

typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

struct FileJob {
    std::string input_path;
    std::string output_path;
    uint8_t key;
    set_key_fn set_key;
    caesar_fn caesar;
};

struct JobQueue {
    std::queue<FileJob> jobs;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool done = false;  // Флаг: задачи закончились
};

struct Logger {
    std::string log_path;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
};

struct ThreadData {
    JobQueue* queue;
    Logger* logger;
    int thread_id;
};

void sigint_handler(int) {
    keep_running = 0;
}

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

void* worker_thread(void* arg) {
    ThreadData* td = static_cast<ThreadData*>(arg);
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    while (keep_running) {
        FileJob job;
        bool has_job = false;
        
        pthread_mutex_lock(&td->queue->mutex);
        
        // Ждём задачу ИЛИ сигнал завершения
        while (td->queue->jobs.empty() && !td->queue->done && keep_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += TIMEOUT_SEC;
            
            int rc = pthread_cond_timedwait(&td->queue->cond, &td->queue->mutex, &ts);
            if (rc == ETIMEDOUT) {
                // Таймаут истёк — проверяем флаги
                if (td->queue->done || !keep_running) {
                    pthread_mutex_unlock(&td->queue->mutex);
                    return nullptr;
                }
                // Если не done — продолжаем ждать
            }
        }
        
        // Если очередь пуста И помечена как завершённая — выходим
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
        
        if (!has_job) {
            break;
        }
        
        // Проверка файла
        struct stat st;
        if (stat(job.input_path.c_str(), &st) != 0) {
            std::cerr << "File not found: " << job.input_path << std::endl;
            write_log(td->logger, td->thread_id, job.input_path, "ERROR");
            continue;
        }
        
        std::ifstream in(job.input_path, std::ios::binary);
        if (!in) {
            std::cerr << "Error opening: " << job.input_path << std::endl;
            write_log(td->logger, td->thread_id, job.input_path, "ERROR");
            continue;
        }
        
        std::ofstream out(job.output_path, std::ios::binary);
        if (!out) {
            std::cerr << "Error creating: " << job.output_path << std::endl;
            write_log(td->logger, td->thread_id, job.input_path, "ERROR");
            continue;
        }
        
        job.set_key(static_cast<char>(job.key));
        
        bool success = true;
        while (keep_running && in) {
            in.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            
            job.caesar(buffer.data(), buffer.data(), static_cast<int>(n));
            out.write(reinterpret_cast<const char*>(buffer.data()), n);
            if (!out) {
                success = false;
                break;
            }
        }
        
        in.close();
        out.close();
        
        if (success && keep_running) {
            std::cout << "Processed: " << job.input_path 
                      << " -> " << job.output_path << std::endl;
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

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: ./secure_copy <input_files...> <output_dir> <key>\n";
        return 1;
    }
    
    std::string out_dir = argv[argc - 2];
    uint8_t key = static_cast<uint8_t>(std::atoi(argv[argc - 1]) & 0xFF);
    
    if (!ensure_dir(out_dir)) {
        std::cerr << "Cannot create output directory: " << out_dir << std::endl;
        return 1;
    }
    
    void* handle = dlopen("./libcaesar.so", RTLD_NOW);
    if (!handle) {
        std::cerr << "Library error: " << dlerror() << std::endl;
        return 1;
    }
    
    auto set_key = reinterpret_cast<set_key_fn>(dlsym(handle, "set_key"));
    auto caesar = reinterpret_cast<caesar_fn>(dlsym(handle, "caesar"));
    if (!set_key || !caesar) {
        std::cerr << "Symbol error: " << dlerror() << std::endl;
        dlclose(handle);
        return 1;
    }
    
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    
    JobQueue queue;
    Logger logger{"log.txt"};
    
    std::ofstream clear_log(logger.log_path, std::ios::trunc);
    clear_log.close();
    
    // Добавляем все задачи в очередь
    for (int i = 1; i < argc - 2; ++i) {
        FileJob job;
        job.input_path = argv[i];
        job.output_path = out_dir + "/" + get_basename(argv[i]);
        job.key = key;
        job.set_key = set_key;
        job.caesar = caesar;
        
        pthread_mutex_lock(&queue.mutex);
        queue.jobs.push(job);
        pthread_mutex_unlock(&queue.mutex);
    }
    
    // Помечаем очередь как завершённую СРАЗУ после добавления задач
    pthread_mutex_lock(&queue.mutex);
    queue.done = true;  // Больше задач не будет
    pthread_cond_broadcast(&queue.cond);  // Будим все потоки
    pthread_mutex_unlock(&queue.mutex);
    
    ThreadData td[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; ++i) {
        td[i].queue = &queue;
        td[i].logger = &logger;
        td[i].thread_id = i;
    }
    
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; ++i) {
        if (pthread_create(&threads[i], nullptr, worker_thread, &td[i]) != 0) {
            std::cerr << "Failed to create thread " << i << std::endl;
            keep_running = 0;
        }
    }
    
    // Ждём завершения всех потоков
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], nullptr);
    }
    
    // Очистка
    pthread_mutex_destroy(&queue.mutex);
    pthread_cond_destroy(&queue.cond);
    pthread_mutex_destroy(&logger.mutex);
    dlclose(handle);
    
    if (!keep_running) {
        std::cout << "Операция прервана пользователем" << std::endl;
        write_log(&logger, -1, "-", "PROGRAM_INTERRUPTED");
        return 130;
    }
    
    std::cout << "Готово. Обработано файлов: " << (argc - 3) << std::endl;
    std::cout << "Лог сохранён в: log.txt" << std::endl;
    return 0;
}