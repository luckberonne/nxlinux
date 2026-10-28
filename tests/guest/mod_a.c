extern long hle_add(long, long, long);
extern double hle_scale(double, float);
extern long not_implemented_import(long);
long g_value = 7;
long (*g_table[])(long) = {0};                 // filled by a RELATIVE/ABS64 relocation below
long twice(long a) { return a * 2; }
long (*g_fn)(long) = twice;                    // a function pointer in data: needs a RELATIVE relocation
long entry(long a) { return hle_add(a, 1, 2) * 2 + g_value; }
long via_pointer(long a) { return g_fn(a) + 1; }
double entry_fp(double a) { return hle_scale(a, 2.0f); }
long call_missing(long a) { return not_implemented_import(a) + 5; }
