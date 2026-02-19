#include <cstdint>

static uint8_t g_key = 0;

extern "C" {

void set_key(char key) {
    g_key = static_cast<uint8_t>(key);
}

void caesar(void* src, void* dst, int len) {
    if (len <= 0 || src == nullptr || dst == nullptr) {
        return;
    }

    const uint8_t* input  = static_cast<const uint8_t*>(src);
    uint8_t*       output = static_cast<uint8_t*>(dst);

    for (int i = 0; i < len; ++i) {
        output[i] = input[i] ^ g_key;
    }
}

}