#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <dlfcn.h>
#include <cstdlib>

// Типы функций из библиотеки
typedef void (*set_key_fn)(char);
typedef void (*caesar_fn)(void*, void*, int);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Incorrect command" << std::endl;
        std::cerr << "Expected: ./test_runner <lib_path> <key> <input_file> <output_file>" << std::endl;
        return 1;
    }

    const char* lib_path   = argv[1];
    int key                = std::atoi(argv[2]) & 0xFF;
    const char* input_file = argv[3];
    const char* output_file= argv[4];

    // Динамическая загрузка библиотеки
    void* handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        std::cerr << "Error loading library: " << dlerror() << std::endl;
        return 1;
    }

    // Получение указателей на функции
    dlerror(); // Очистка ошибок
    set_key_fn set_key = (set_key_fn)dlsym(handle, "set_key");
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        std::cerr << "Error loading symbol set_key: " << dlsym_error << std::endl;
        dlclose(handle);
        return 1;
    }

    caesar_fn caesar = (caesar_fn)dlsym(handle, "caesar");
    dlsym_error = dlerror();
    if (dlsym_error) {
        std::cerr << "Error loading symbol caesar: " << dlsym_error << std::endl;
        dlclose(handle);
        return 1;
    }

    // Чтение входного файла (бинарно)
    std::ifstream infile(input_file, std::ios::binary | std::ios::ate);
    if (!infile) {
        std::cerr << "Error opening input file: " << input_file << std::endl;
        dlclose(handle);
        return 1;
    }

    std::streamsize size = infile.tellg();
    infile.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!infile.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Error reading input file" << std::endl;
        dlclose(handle);
        return 1;
    }
    infile.close();

    // Вызов функций библиотеки
    // set_key принимает char, приводим int к char
    set_key(static_cast<char>(key));
    
    // caesar(src, dst, len)
    caesar(buffer.data(), buffer.data(), static_cast<int>(size));

    // Запись выходного файла (бинарно)
    std::ofstream outfile(output_file, std::ios::binary);
    if (!outfile) {
        std::cerr << "Error opening output file: " << output_file << std::endl;
        dlclose(handle);
        return 1;
    }

    outfile.write(reinterpret_cast<const char*>(buffer.data()), size);
    outfile.close();

    // Очистка
    dlclose(handle);

    return 0;
}