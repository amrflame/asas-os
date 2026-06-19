#include "asas_cpp.hpp"

extern "C" void cpp_user_main()
{
    UINT8 heap[2048];
    asas::StaticVector<UINT64, 4> values;

    asas_heap_initialize(heap, sizeof(heap));
    asas::HeapBuffer buffer(64);

    if (!buffer.valid() || !values.push_back(0x41534153ULL) || values[0] != 0x41534153ULL) {
        asas_exit(1);
    }

    asas_write("Hello from an Asas OS C++ user program");
    asas_exit(0);
}

