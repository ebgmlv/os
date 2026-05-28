#include <cstdint>

extern "C" {

struct RC4State {
    uint8_t S[256];
    int i, j;
};

// Инициализация состояния RC4
void* rc4_init(const uint8_t* key, int keylen) {
    RC4State* state = new RC4State;
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
    return state;
}

// Поточное шифрование/дешифрование
void rc4_crypt(void* ctx, void* src, void* dst, int len) {
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
}

// Очистка выделенной памяти
void rc4_free(void* ctx) {
    delete static_cast<RC4State*>(ctx);
}

}