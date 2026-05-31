#include <cstdint>
#include <sys/mman.h>  // [НОВОЕ] Для mmap, mprotect, munmap
#include <unistd.h>    // [НОВОЕ] Для sysconf

extern "C" {

struct RC4State {
    uint8_t S[256];
    int i, j;
};

// Инициализация состояния RC4
void* rc4_init(const uint8_t* key, int keylen) {
    // [ИЗМЕНЕНО] Вместо new RC4State используем mmap для выделения целой страницы памяти
    long page_size = sysconf(_SC_PAGESIZE);
    void* mem = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    
    RC4State* state = static_cast<RC4State*>(mem);
    
    for (int i = 0; i < 256; i++) {
        state->S[i] = i;
    }
    state->i = 0; 
    state->j = 0;
    
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + state->S[i] + key[i % keylen]) % 256;
        uint8_t tmp = state->S[i];
        state->S[i] = state->S[j];
        state->S[j] = tmp;
    }
    
    // [НОВОЕ] Блокируем доступ к S-box после инициализации
    mprotect(mem, page_size, PROT_NONE);
    return mem;
}

// Поточное шифрование/дешифрование
void rc4_crypt(void* ctx, void* src, void* dst, int len) {
    if (!ctx) return;
    long page_size = sysconf(_SC_PAGESIZE);
    
    // [НОВОЕ] Открываем память только на время шифрования
    mprotect(ctx, page_size, PROT_READ | PROT_WRITE);

    RC4State* state = static_cast<RC4State*>(ctx);
    uint8_t* input = static_cast<uint8_t*>(src);
    uint8_t* output = static_cast<uint8_t*>(dst);
    
    for (int k = 0; k < len; k++) {
        state->i = (state->i + 1) % 256;
        state->j = (state->j + state->S[state->i]) % 256;
        
        uint8_t tmp = state->S[state->i];
        state->S[state->i] = state->S[state->j];
        state->S[state->j] = tmp;
        
        uint8_t K = state->S[(state->S[state->i] + state->S[state->j]) % 256];
        output[k] = input[k] ^ K;
    }
    
    // [НОВОЕ] Снова блокируем доступ после завершения работы с файлом
    mprotect(ctx, page_size, PROT_NONE);
}

// Очистка выделенной памяти
void rc4_free(void* ctx) {
    if (!ctx) return;
    long page_size = sysconf(_SC_PAGESIZE);
    
    // [НОВОЕ] Открываем память, чтобы затереть ключи нулями перед удалением
    mprotect(ctx, page_size, PROT_READ | PROT_WRITE);
    
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ctx);
    for (size_t i = 0; i < sizeof(RC4State); ++i) {
        p[i] = 0; // Secure wipe
    }

    // [ИЗМЕНЕНО] Вместо delete используем munmap
    munmap(ctx, page_size);
}

}