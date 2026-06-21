#ifndef FACE_DB_H
#define FACE_DB_H

// Simple flat-file face database for door-access recognition.
//
// Layout on disk (binary, little-endian):
//   4 bytes  : int32  count
//   count × (FACE_DB_NAME_LEN + FACE_DB_EMBED_DIM × 4) bytes : entries
//
// Thread-safety: not thread-safe; single-threaded use only.

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_DB_MAX_ENTRIES 64
#define FACE_DB_NAME_LEN    64
#define FACE_DB_EMBED_DIM   128

typedef struct {
    char  name[FACE_DB_NAME_LEN];   /* null-terminated display name */
    float embedding[FACE_DB_EMBED_DIM];
} face_entry_t;

typedef struct {
    int          count;
    face_entry_t entries[FACE_DB_MAX_ENTRIES];
} face_db_t;

/* Load database from file.
 * Returns 0 on success; -1 if file not found (db is left empty, not an error
 * for first-time use).  On corrupt data the loaded count is clamped. */
int face_db_load(face_db_t *db, const char *path);

/* Persist database to file.  Returns 0 on success, -1 on I/O error. */
int face_db_save(const face_db_t *db, const char *path);

/* Append a new entry.  Returns 0 on success, -1 if the database is full. */
int face_db_add(face_db_t *db, const char *name, const float *embedding);

/* Find the closest registered face.
 * Sets *out_dist to the L2 distance.
 * Returns the index of the best match, or -1 if the database is empty. */
int face_db_find(const face_db_t *db,
                 const float     *embedding,
                 float           *out_dist);

/* Print a summary of all registered entries to stdout. */
void face_db_print(const face_db_t *db);

#ifdef __cplusplus
}
#endif

#endif /* FACE_DB_H */
