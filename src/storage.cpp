#include "storage.h"
struct FILE_STRUCTURE {

};
void *openFile(const char *path) {
    auto ptr = new FILE_STRUCTURE;
    return ptr;
}
void closeFile(void *&ptr) {
    delete ptr;
    ptr = nullptr;
}