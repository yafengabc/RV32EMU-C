/*
 * embed.h - build the emulator as a DLL so a host (gui/leakrun) can start and
 * stop full guest sessions in one process.
 *
 *   _init()            argv-style option parsing without a program name
 *   _run(config)       embedded config language (see gui/README.md)
 *   _shutdown()        release everything _init()/_run() allocated
 *   _last_error()      last configuration error, "" when none
 */
#ifndef EMBED_H
#define EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BUILDING_EMU
#define EMBED_API __declspec(dllexport)
#else
#define EMBED_API __declspec(dllimport)
#endif

EMBED_API void        _init(void);
EMBED_API int         _run(const char *config);
EMBED_API void        _shutdown(void);
EMBED_API const char *_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBED_H */
