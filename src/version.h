/* The only place the version is set; the code, the Windows resource and the
   build scripts all read it from here. */
#define PF_VER_MAJOR 0
#define PF_VER_MINOR 2
#define PF_VER_PATCH 0

#define PF_STR_(x) #x
#define PF_STR(x) PF_STR_(x)
#define PF_VERSION PF_STR(PF_VER_MAJOR.PF_VER_MINOR)
#define PF_VERSION_FULL PF_STR(PF_VER_MAJOR.PF_VER_MINOR.PF_VER_PATCH)
#define PF_CAT_(a, b) a##b
#define PF_CAT(a, b) PF_CAT_(a, b)
#define PF_EXE_NAME PF_STR(PF_CAT(PlaneForge_v, PF_VER_MAJOR).PF_VER_MINOR.exe)
