/* inst.h - work out what a downloaded file is (installer flavour or archive) */
#ifndef INST_H
#define INST_H

typedef enum {
	INST_UNKNOWN = 0,
	INST_MSI, INST_NSIS, INST_INNO, INST_BURN, INST_INSTALLSHIELD, INST_SQUIRREL, INST_7ZIP,
	INST_EXE,                       /* a PE with no known installer marker */
	INST_ZIP, INST_NUPKG, INST_APPX,
	INST_7Z, INST_TAR, INST_GZIP, INST_BZIP2, INST_XZ,
	INST_SFX                        /* 7-Zip / RAR self-extracting EXE: archives, from INST_7Z on */
} InstType;

/* wide: match as UTF-16LE. weak: only counts when no non-weak marker is found anywhere in the file */
typedef struct { InstType type; const char *marker; int wide; int weak; } InstMarker;
typedef struct { InstType type; const char *install; const char *uninstall; } InstArgs;

/* path: the file. name_hint: its URL or name, only used for .nupkg/.appx/.msix and as a last resort */
InstType    inst_detect(const char *path, const char *name_hint);
/* "nsis", "inno", "msi", ... (case-insensitive) -> type, INST_UNKNOWN if not a name we know */
InstType    inst_from_name(const char *s);
const char *inst_name(InstType t);
/* quiet switches from the inst_args table in config.h, NULL if none are known */
const char *inst_quiet_args(InstType t, int uninstall);

#endif
