#include "face_db.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

int face_db_load(face_db_t *db, const char *path)
{
    memset(db, 0, sizeof(face_db_t));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;   /* not found – caller treats as empty DB */

    if (fread(&db->count, sizeof(int), 1, f) != 1) {
        fclose(f);
        db->count = 0;
        return -1;
    }
    if (db->count < 0 || db->count > FACE_DB_MAX_ENTRIES)
        db->count = 0;

    size_t n = (size_t)db->count;
    if (n > 0 && fread(db->entries, sizeof(face_entry_t), n, f) != n) {
        db->count = 0;
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

int face_db_save(const face_db_t *db, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[face_db] Cannot open %s for writing\n", path);
        return -1;
    }

    fwrite(&db->count, sizeof(int), 1, f);
    if (db->count > 0)
        fwrite(db->entries, sizeof(face_entry_t), (size_t)db->count, f);

    fclose(f);
    return 0;
}

int face_db_add(face_db_t *db, const char *name, const float *embedding)
{
    if (db->count >= FACE_DB_MAX_ENTRIES) {
        printf("[face_db] Database full (%d entries)\n", FACE_DB_MAX_ENTRIES);
        return -1;
    }

    face_entry_t *e = &db->entries[db->count];
    strncpy(e->name, name, FACE_DB_NAME_LEN - 1);
    e->name[FACE_DB_NAME_LEN - 1] = '\0';
    memcpy(e->embedding, embedding, FACE_DB_EMBED_DIM * sizeof(float));

    db->count++;
    return 0;
}

int face_db_find(const face_db_t *db,
                 const float     *embedding,
                 float           *out_dist)
{
    if (db->count == 0) {
        *out_dist = 9999.0f;
        return -1;
    }

    int   best_idx  = 0;
    float best_dist = 9999.0f;

    for (int i = 0; i < db->count; i++) {
        float sum = 0.0f;
        for (int j = 0; j < FACE_DB_EMBED_DIM; j++) {
            float d = embedding[j] - db->entries[i].embedding[j];
            sum += d * d;
        }
        float dist = sqrtf(sum);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = i;
        }
    }

    *out_dist = best_dist;
    return best_idx;
}

void face_db_print(const face_db_t *db)
{
    printf("[face_db] %d registered face(s):\n", db->count);
    for (int i = 0; i < db->count; i++)
        printf("  [%d] %s\n", i, db->entries[i].name);
}
