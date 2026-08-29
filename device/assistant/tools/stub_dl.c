void *dlopen(const char *f, int m){(void)f;(void)m;return 0;}
void *dlsym(void *h, const char *s){(void)h;(void)s;return 0;}
int dlclose(void *h){(void)h;return 0;}
const char *dlerror(void){return 0;}
