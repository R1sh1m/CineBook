/*
 * seed.c — CineBook Database Seeder (STRUCTURAL DATA ONLY)
 * ===========================================================================
 * Seeds:
 *   countries, cities, academic_domains, refund_policy, users,
 *   theatres, screens, seats, promos
 *
 * Creates empty placeholders:
 *   movies.db, cast_members.db, shows.db, seat_status.db,
 *   bookings.db, booking_seats.db, payments.db, refunds.db, waitlist.db
 *
 * Compile and run ONCE before starting cinebook:
 *   gcc -std=c11 -Wall -o seed seed.c && ./seed
 *
 * Then move outputs:
 *   mv *.db data/db/  &&  mv *.idx data/idx/  &&  mv wal.log data/
 *
 * Page layout (4096 bytes):
 *   [0..3]   page_id       uint32_t  little-endian
 *   [4..5]   record_count  uint16_t  little-endian
 *   [6..7]   free_offset   uint16_t  little-endian
 *   [8..4095] record data  fixed-size slots, NUL-padded
 * ===========================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* ── Page constants ──────────────────────────────────────────────────────── */
#define PAGE_SIZE        4096
#define PAGE_HEADER_SIZE 8
#define PAGE_DATA_SIZE   (PAGE_SIZE - PAGE_HEADER_SIZE)

/* ── NULL sentinels ──────────────────────────────────────────────────────── */
#define INT_NULL    ((int32_t)0x80000000)
#define FLOAT_NULL  0xFFFFFFFFu

/* ── Record sizes (MUST match schema.cat exactly) ───────────────────────── */
#define RS_USERS            374
#define RS_MOVIES           896
#define RS_CAST_MEMBERS     312
#define RS_THEATRES         462
#define RS_SCREENS           86
#define RS_SEATS             23
#define RS_SHOWS             40
#define RS_SEAT_STATUS       44
#define RS_BOOKINGS          88
#define RS_BOOKING_SEATS     16
#define RS_PAYMENTS         160
#define RS_REFUNDS          252
#define RS_PROMOS            96
#define RS_WAITLIST          60
#define RS_COUNTRIES        112
#define RS_CITIES           108
#define RS_ACADEMIC_DOMAINS 104
#define RS_REFUND_POLICY     62

/* ── Index entries (must match runtime index binary layout exactly) ─────── */
typedef struct {
    int32_t  key;
    uint32_t page_id;
    uint16_t slot_id;
    int16_t  is_deleted;
} HashEntryDisk;

typedef struct {
    int32_t  key;
    char     sort_key[32];
    uint32_t page_id;
    uint16_t slot_id;
    int16_t  is_deleted;
} SortedIndexEntryDisk;

/* Seeder-internal compact entry representation */
typedef struct {
    int32_t key;
    uint32_t page_id;
    uint16_t slot_id;
} IdxEntry;

/* ── In-memory table ─────────────────────────────────────────────────────── */
typedef struct {
    const char *filename;
    int         record_size;
    uint8_t    *data;
    int         count;
    int         capacity;
} Table;

/* ── In-memory index ─────────────────────────────────────────────────────── */
typedef struct {
    const char *filename;
    uint8_t     type;     /* 0=hash 1=sorted */
    IdxEntry   *entries;
    int         count;
    int         capacity;
} Index;

/* =========================================================================
 * Utility helpers
 * ========================================================================= */
static void die(const char *msg) {
    fprintf(stderr, "SEED ERROR: %s — %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static void put_i32(uint8_t *b, int p, int32_t v) {
    b[p+0]=(uint8_t)(v&0xFF); b[p+1]=(uint8_t)((v>>8)&0xFF);
    b[p+2]=(uint8_t)((v>>16)&0xFF); b[p+3]=(uint8_t)((v>>24)&0xFF);
}
static void put_u16(uint8_t *b, int p, uint16_t v) {
    b[p+0]=(uint8_t)(v&0xFF); b[p+1]=(uint8_t)((v>>8)&0xFF);
}
static void put_f32(uint8_t *b, int p, float v) {
    uint32_t u; memcpy(&u,&v,4); put_i32(b,p,(int32_t)u);
}
static void put_f32_null(uint8_t *b, int p) {
    uint32_t u=FLOAT_NULL; memcpy(b+p,&u,4);
}
static void put_i32_null(uint8_t *b, int p) { put_i32(b,p,INT_NULL); }
static void put_str(uint8_t *b, int p, int sz, const char *s) {
    memset(b+p,0,(size_t)sz);
    if(s&&s[0]){size_t l=strlen(s);if((int)l>sz)l=(size_t)sz;memcpy(b+p,s,l);}
}
static void put_date(uint8_t *b, int p, const char *dt) {
    memset(b+p,0,20);
    size_t _l=strlen(dt);
    if(_l>19)_l=19;
    memcpy(b+p,dt,_l);
}

static void write_empty_page_file(const char *filename)
{
    char path[256];
    snprintf(path, sizeof(path), "data/db/%s", filename);
    FILE *f = fopen(path, "wb");
    if (!f) die(path);

    uint8_t page[PAGE_SIZE];
    memset(page, 0, sizeof(page));

    if (fwrite(page, PAGE_SIZE, 1, f) != 1) die("fwrite empty page");
    fclose(f);
    printf("  wrote %-35s  %d record(s) [placeholder]\n", filename, 0);
}

/* =========================================================================
 * Table helpers
 * ========================================================================= */
static Table table_new(const char *fn, int rs) {
    Table t; t.filename=fn; t.record_size=rs; t.count=0; t.capacity=512;
    t.data=calloc((size_t)t.capacity,(size_t)rs);
    if(!t.data) die("calloc table");
    return t;
}
static uint8_t *table_add(Table *t) {
    if(t->count>=t->capacity){
        t->capacity*=2;
        t->data=realloc(t->data,(size_t)t->capacity*(size_t)t->record_size);
        if(!t->data) die("realloc table");
        int half=t->capacity/2;
        memset(t->data+(size_t)half*(size_t)t->record_size,0,
               (size_t)half*(size_t)t->record_size);
    }
    uint8_t *r=t->data+(size_t)t->count*(size_t)t->record_size;
    t->count++; return r;
}
static void table_flush(const Table *t) {
    char _tpath[256]; snprintf(_tpath,sizeof(_tpath),"data/db/%s",t->filename);
    FILE *f=fopen(_tpath,"wb"); if(!f) die(_tpath);
    int spp=PAGE_DATA_SIZE/t->record_size;
    int left=t->count, idx=0, pid=0;
    do {
        uint8_t page[PAGE_SIZE]; memset(page,0,PAGE_SIZE);
        int rc=left<spp?left:spp;
        put_i32(page,0,pid); put_u16(page,4,(uint16_t)rc);
        put_u16(page,6,(uint16_t)(rc*t->record_size));
        for(int s=0;s<rc;s++)
            memcpy(page+PAGE_HEADER_SIZE+s*t->record_size,
                   t->data+(size_t)(idx+s)*(size_t)t->record_size,(size_t)t->record_size);
        if(fwrite(page,PAGE_SIZE,1,f)!=1) die("fwrite page");
        idx+=rc; left-=rc; pid++;
    } while(left>0);
    fclose(f);
    printf("  wrote %-35s  %d record(s)\n",t->filename,t->count);
}
static void table_free(Table *t){free(t->data);t->data=NULL;}

/* =========================================================================
 * Index helpers
 * ========================================================================= */
static Index index_new(const char *fn, uint8_t type) {
    Index ix; ix.filename=fn; ix.type=type; ix.count=0; ix.capacity=1024;
    ix.entries=malloc((size_t)ix.capacity*sizeof(IdxEntry));
    if(!ix.entries) die("malloc index");
    return ix;
}
static void index_add(Index *ix, int32_t key, int32_t pg, int32_t sl) {
    if(ix->count>=ix->capacity){
        ix->capacity*=2;
        ix->entries=realloc(ix->entries,(size_t)ix->capacity*sizeof(IdxEntry));
        if(!ix->entries) die("realloc index");
    }
    ix->entries[ix->count].key=key;
    ix->entries[ix->count].page_id=pg;
    ix->entries[ix->count].slot_id=sl;
    ix->count++;
}
static int cmp_idx(const void *a,const void *b){
    const IdxEntry*ea=(const IdxEntry*)a,*eb=(const IdxEntry*)b;
    if(ea->key<eb->key)return -1;
    if(ea->key>eb->key)return 1;
    return 0;
}
static void index_flush(Index *ix, int rs) {
    if(ix->type==1) qsort(ix->entries,(size_t)ix->count,sizeof(IdxEntry),cmp_idx);
    char _ipath[256]; snprintf(_ipath,sizeof(_ipath),"data/idx/%s",ix->filename);
    FILE *f=fopen(_ipath,"wb"); if(!f) die(_ipath);

    if (ix->type == 0) {
        for (int i = 0; i < ix->count; i++) {
            HashEntryDisk e;
            memset(&e, 0, sizeof(e));
            e.key        = ix->entries[i].key;
            e.page_id    = ix->entries[i].page_id;
            e.slot_id    = ix->entries[i].slot_id;
            e.is_deleted = 0;
            if (fwrite(&e, sizeof(HashEntryDisk), 1, f) != 1) die("fwrite hash idx entry");
        }
    } else {
        for (int i = 0; i < ix->count; i++) {
            SortedIndexEntryDisk e;
            memset(&e, 0, sizeof(e));
            e.key        = ix->entries[i].key;
            e.page_id    = ix->entries[i].page_id;
            e.slot_id    = ix->entries[i].slot_id;
            e.is_deleted = 0;
            if (fwrite(&e, sizeof(SortedIndexEntryDisk), 1, f) != 1) die("fwrite sorted idx entry");
        }
    }

    fclose(f);
    printf("  wrote %-35s  %d entr%s  [%s]\n",ix->filename,ix->count,
           ix->count==1?"y":"ies",ix->type==0?"hash":"sorted");
    (void)rs;
}
static void index_free(Index *ix){free(ix->entries);ix->entries=NULL;}

static void record_location(int ri, int rs, int32_t *pg, int32_t *sl) {
    int spp=PAGE_DATA_SIZE/rs;
    *pg=(int32_t)(ri/spp); *sl=(int32_t)(ri%spp);
}

/* =========================================================================
 * Credential hashes (pre-computed SHA-256 hex)
 * ========================================================================= */
#define HASH_ADMIN123  "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9"
#define HASH_PASS1234  "bd94dcda26fccb4e68d6a31f9b5aac0b571ae266d822620e901ef7ebe3a11d4f"
#define HASH_CINEMA99  "440cde37d7ccbd5596dc9a99e9a8a8253a021d72e888f340618e6cb41d169872"
#define HASH_STUDENT1  "509e87a6c45ee0a3c657bf946dd6dc43d7e5502143be195280f279002e70f7d9"

/* =========================================================================
 * Theatre seed data — 17 theatres, 7 cities
 * ========================================================================= */
typedef struct { int id; const char *name; int city_id; const char *address; } TheatreSeed;
static const TheatreSeed THEATRES[] = {
    {1,"PVR Cinemas Velachery",     1,"Phoenix MarketCity, Velachery, Chennai 600042"},
    {2,"INOX Express Avenue",       1,"Express Avenue Mall, Royapettah, Chennai 600002"},
    {3,"Rohini Silver Screens",     1,"Rohini Theatre Complex, Koyambedu, Chennai 600107"},
    {4,"SPI Palazzo Cinemas",       1,"Anna Salai, Egmore, Chennai 600008"},
    {5,"Cinepolis Andheri West",    2,"Infiniti Mall, Andheri West, Mumbai 400053"},
    {6,"PVR Juhu",                  2,"Juhu Shopping Centre, Juhu, Mumbai 400049"},
    {7,"INOX R-City Ghatkopar",     2,"R-City Mall, Ghatkopar West, Mumbai 400086"},
    {8,"PVR Select Citywalk",       3,"Select Citywalk Mall, Saket, New Delhi 110017"},
    {9,"DT Cinemas Saket",          3,"DT City Centre Mall, Saket, New Delhi 110017"},
    {10,"PVR Forum Mall",           4,"Forum Mall, Koramangala, Bengaluru 560095"},
    {11,"INOX Garuda Mall",         4,"Garuda Mall, Magrath Road, Bengaluru 560025"},
    {12,"Cinepolis Orion Mall",     4,"Orion Mall, Rajajinagar, Bengaluru 560010"},
    {13,"AMB Cinemas Gachibowli",   5,"AMB Mall, Gachibowli, Hyderabad 500032"},
    {14,"Prasads IMAX",             5,"NTR Gardens, Lower Tank Bund, Hyderabad 500080"},
    {15,"INOX South City",          6,"South City Mall, Prince Anwar Shah Rd, Kolkata 700068"},
    {16,"PVR Mani Square",          6,"Mani Square Mall, EM Bypass, Kolkata 700065"},
    {17,"Cinepolis Amanora",        7,"Amanora Town Centre, Hadapsar, Pune 411028"},
};
#define N_THEATRES ((int)(sizeof(THEATRES)/sizeof(THEATRES[0])))

/* =========================================================================
 * Screen seed data — 38 screens
 * screen_type: 0=2D  1=IMAX_2D  2=IMAX_3D  3=4DX
 * ========================================================================= */
typedef struct {
    int id; int theatre_id; const char *name; int stype;
    int rows; int cols; int rec_end; int prem_end;
} ScreenSeed;

static const ScreenSeed SCREENS[] = {
    { 1, 1,"Screen 1",        0, 10, 15, 2, 3},
    { 2, 1,"IMAX Hall",       2, 12, 18, 2, 3},
    { 3, 1,"4DX Screen",      3,  8, 10, 0, 3},
    { 4, 2,"Screen 1",        0, 10, 14, 2, 2},
    { 5, 2,"IMAX 2D Hall",    1, 12, 16, 2, 3},
    { 6, 3,"Silver Screen",   0, 12, 18, 0, 4},
    { 7, 3,"Gold Screen",     0, 10, 14, 2, 3},
    { 8, 3,"Mini Screen",     0,  8, 12, 0, 3},
    { 9, 4,"Screen 1",        0, 10, 15, 2, 3},
    {10, 4,"4DX Premium",     3,  8, 10, 0, 3},
    {11, 5,"Screen A",        0, 10, 15, 2, 3},
    {12, 5,"IMAX Screen",     2, 14, 18, 2, 4},
    {13, 6,"Screen 1",        0, 10, 14, 2, 2},
    {14, 6,"Screen 2",        0, 10, 12, 0, 3},
    {15, 7,"Screen 1",        0, 10, 15, 2, 3},
    {16, 7,"IMAX 2D",         1, 12, 16, 2, 3},
    {17, 8,"Screen 1",        0, 10, 15, 2, 3},
    {18, 8,"IMAX Hall",       2, 12, 18, 2, 3},
    {19, 9,"Screen 1",        0, 10, 14, 0, 4},
    {20, 9,"Screen 2",        0,  8, 12, 0, 3},
    {21,10,"Screen 1",        0, 10, 15, 2, 3},
    {22,10,"IMAX 2D",         1, 12, 16, 2, 3},
    {23,11,"Screen 1",        0, 10, 14, 2, 2},
    {24,11,"Screen 2",        0,  8, 12, 0, 3},
    {25,12,"Screen 1",        0, 10, 15, 2, 3},
    {26,12,"4DX Screen",      3,  8, 10, 0, 3},
    {27,13,"Screen 1",        0, 10, 15, 2, 3},
    {28,13,"Screen 2",        0, 10, 14, 0, 4},
    {29,13,"IMAX Hall",       2, 12, 18, 2, 3},
    {30,14,"IMAX 2D Grand",   1, 14, 20, 2, 3},
    {31,14,"IMAX 3D Grand",   2, 14, 20, 2, 3},
    {32,14,"Screen 3",        0, 10, 15, 2, 3},
    {33,15,"Screen 1",        0, 10, 15, 2, 3},
    {34,15,"IMAX 2D",         1, 12, 16, 2, 3},
    {35,16,"Screen 1",        0, 10, 14, 0, 4},
    {36,16,"Screen 2",        0,  8, 12, 0, 3},
    {37,17,"Screen 1",        0, 10, 15, 2, 3},
    {38,17,"IMAX 2D",         1, 12, 16, 2, 3},
};
#define N_SCREENS ((int)(sizeof(SCREENS)/sizeof(SCREENS[0])))

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("CineBook Seeder (STRUCTURAL) — theatres/screens/seats only\n");
    printf("============================================================\n\n");

    /* ── [1] countries ──────────────────────────────────────────────────── */
    printf("[1/18] countries\n");
    Table tCountries = table_new("countries.db", RS_COUNTRIES);
    {
        uint8_t *r = table_add(&tCountries);
        put_i32(r,  0, 1);
        put_str(r,  4,100,"India");
        put_str(r,104,  4,"Rs.");
        put_str(r,108,  4,"INR");
    }
    table_flush(&tCountries);
    Index ixCountriesPK = index_new("countries_id.idx",0);
    { int32_t pg,sl; record_location(0,RS_COUNTRIES,&pg,&sl); index_add(&ixCountriesPK,1,pg,sl); }
    index_flush(&ixCountriesPK,RS_COUNTRIES);
    table_free(&tCountries); index_free(&ixCountriesPK);

    /* ── [2] cities ─────────────────────────────────────────────────────── */
    printf("\n[2/18] cities\n");
    typedef struct { int id; const char *name; } CitySeed;
    static const CitySeed CITIES[] = {
        {1,"Chennai"},{2,"Mumbai"},{3,"Delhi"},{4,"Bengaluru"},
        {5,"Hyderabad"},{6,"Kolkata"},{7,"Pune"},{8,"Ahmedabad"},
        {9,"Jaipur"},{10,"Kochi"},
    };
    int n_cities=(int)(sizeof(CITIES)/sizeof(CITIES[0]));
    Table tCities=table_new("cities.db",RS_CITIES);
    Index ixCitiesPK=index_new("cities_id.idx",0);
    Index ixCitiesName=index_new("cities_name.idx",1);
    for(int i=0;i<n_cities;i++){
        uint8_t *r=table_add(&tCities);
        put_i32(r,  0,(int32_t)CITIES[i].id);
        put_str(r,  4,100,CITIES[i].name);
        put_i32(r,104,1);
        int32_t pg,sl; record_location(i,RS_CITIES,&pg,&sl);
        index_add(&ixCitiesPK,CITIES[i].id,pg,sl);
        index_add(&ixCitiesName,CITIES[i].id,pg,sl);
    }
    table_flush(&tCities);
    index_flush(&ixCitiesPK,RS_CITIES); index_flush(&ixCitiesName,RS_CITIES);
    table_free(&tCities); index_free(&ixCitiesPK); index_free(&ixCitiesName);

    /* ── [3] academic_domains ───────────────────────────────────────────── */
    printf("\n[3/18] academic_domains\n");
    static const char *DOMAINS[]={
        "iitm.ac.in","iitb.ac.in","iitd.ac.in","iitk.ac.in","iisc.ac.in",
        "bits-pilani.ac.in","vit.ac.in","srm.edu.in","anna.edu.in","nitt.edu",
        "nitc.ac.in","iimb.ac.in","iima.ac.in","tiss.edu","du.ac.in",
        "hcu.ac.in","manipal.edu","psgtech.ac.in","cit.edu.in","ssn.edu.in",
    };
    int n_domains=(int)(sizeof(DOMAINS)/sizeof(DOMAINS[0]));
    Table tDom=table_new("academic_domains.db",RS_ACADEMIC_DOMAINS);
    for(int i=0;i<n_domains;i++){
        uint8_t *r=table_add(&tDom);
        put_i32(r,0,(int32_t)(i+1)); put_str(r,4,100,DOMAINS[i]);
    }
    table_flush(&tDom); table_free(&tDom);

    /* ── [4] refund_policy ──────────────────────────────────────────────── */
    printf("\n[4/18] refund_policy\n");
    typedef struct{int id;int hours;int pct;const char*label;}RefPol;
    static const RefPol REFPOL[]={
        {1,9999,100,"Full Refund"},{2,72,75,"Partial Refund"},
        {3,24,50,"Late Cancellation"},{4,6,0,"No Refund"},
    };
    Table tRefPol=table_new("refund_policy.db",RS_REFUND_POLICY);
    for(int i=0;i<4;i++){
        uint8_t *r=table_add(&tRefPol);
        put_i32(r, 0,(int32_t)REFPOL[i].id);
        put_i32(r, 4,(int32_t)REFPOL[i].hours);
        put_i32(r, 8,(int32_t)REFPOL[i].pct);
        put_str(r,12, 50,REFPOL[i].label);
    }
    table_flush(&tRefPol); table_free(&tRefPol);

    /* ── [5] users ──────────────────────────────────────────────────────── */
    printf("\n[5/18] users\n");
    typedef struct{
        int id;const char*name,*phone,*email,*hash;int role;float wallet;int city_id;
    } UserSeed;
    static const UserSeed USERS[]={
        {1,"Admin CineBook","9000000001","admin@cinebook.in",        HASH_ADMIN123,3,5000.0f,1},
        {2,"Arjun Sharma",  "9876543210","arjun.sharma@gmail.com",   HASH_PASS1234,1, 500.0f,1},
        {3,"Priya Nair",    "9845123456","priya.nair@iitm.ac.in",    HASH_PASS1234,2, 250.0f,1},
        {4,"Rahul Mehta",   "9123456789","rahul.mehta@outlook.com",  HASH_CINEMA99,1,   0.0f,2},
        {5,"Sneha Patel",   "9988776655","sneha.patel@vit.ac.in",    HASH_STUDENT1,2,1200.0f,4},
        {6,"Karthik Rajan", "9090909090","karthik.r@gmail.com",      HASH_PASS1234,1, 750.0f,1},
        {7,"Meera Krishnan","8877665544","meera.k@bits-pilani.ac.in",HASH_STUDENT1,2, 300.0f,5},
    };
    int n_users=(int)(sizeof(USERS)/sizeof(USERS[0]));
    Table tUsers=table_new("users.db",RS_USERS);
    Index ixUsersId=index_new("users_id.idx",0);
    for(int i=0;i<n_users;i++){
        const UserSeed *u=&USERS[i];
        uint8_t *r=table_add(&tUsers);
        put_i32(r,  0,(int32_t)u->id);
        put_str(r,  4,100,u->name);
        put_str(r,104, 15,u->phone);
        if(u->email&&u->email[0]) put_str(r,119,150,u->email);
        put_str(r,269, 65,u->hash);
        put_i32(r,334,(int32_t)u->role);
        put_f32(r,338,u->wallet);
        put_i32(r,342,(int32_t)u->city_id);
        put_i32(r,346,1);
        put_date(r,350,"2024-01-15 00:00");
        put_i32(r,370,1);
        int32_t pg,sl; record_location(i,RS_USERS,&pg,&sl);
        index_add(&ixUsersId,u->id,pg,sl);
    }
    table_flush(&tUsers); index_flush(&ixUsersId,RS_USERS);
    table_free(&tUsers); index_free(&ixUsersId);

    /* ── [6] movies (placeholder empty page) ───────────────────────────── */
    printf("\n[6/18] movies (placeholder)\n");
    write_empty_page_file("movies.db");
    {
        Index ixMoviesId = index_new("movies_id.idx", 0);
        index_flush(&ixMoviesId, RS_MOVIES);
        index_free(&ixMoviesId);
    }

    /* ── [7] cast_members (placeholder empty page) ─────────────────────── */
    printf("\n[7/18] cast_members (placeholder)\n");
    write_empty_page_file("cast_members.db");

    /* ── [8] theatres ───────────────────────────────────────────────────── */
    printf("\n[8/18] theatres  (%d)\n",N_THEATRES);
    Table tTheatres=table_new("theatres.db",RS_THEATRES);
    Index ixTheatreCity=index_new("theatres_cityid.idx",1);
    for(int i=0;i<N_THEATRES;i++){
        const TheatreSeed *t=&THEATRES[i];
        uint8_t *r=table_add(&tTheatres);
        put_i32(r,  0,(int32_t)t->id);
        put_str(r,  4,150,t->name);
        put_i32(r,154,(int32_t)t->city_id);
        put_str(r,158,300,t->address);
        put_i32(r,458,1);
        int32_t pg,sl; record_location(i,RS_THEATRES,&pg,&sl);
        index_add(&ixTheatreCity,t->city_id,pg,sl);
    }
    table_flush(&tTheatres); index_flush(&ixTheatreCity,RS_THEATRES);
    table_free(&tTheatres); index_free(&ixTheatreCity);

    /* ── [9] screens ─────────────────────────────────────────────────────── */
    printf("\n[9/18] screens  (%d)\n",N_SCREENS);
    Table tScreens=table_new("screens.db",RS_SCREENS);
    for(int i=0;i<N_SCREENS;i++){
        const ScreenSeed *sc=&SCREENS[i];
        int total=sc->rows*sc->cols;
        uint8_t *r=table_add(&tScreens);
        put_i32(r, 0,(int32_t)sc->id);
        put_i32(r, 4,(int32_t)sc->theatre_id);
        put_str(r, 8, 50,sc->name);
        put_i32(r,58,(int32_t)sc->stype);
        put_i32(r,62,(int32_t)total);
        put_i32(r,66,(int32_t)sc->rows);
        put_i32(r,70,(int32_t)sc->cols);
        put_i32(r,74,(int32_t)sc->rec_end);
        put_i32(r,78,(int32_t)sc->prem_end);
        put_i32(r,82,1);
    }
    table_flush(&tScreens); table_free(&tScreens);

    /* ── [10] seats ──────────────────────────────────────────────────────── */
    printf("\n[10/18] seats\n");
    Table tSeats=table_new("seats.db",RS_SEATS);
    int seat_id_ctr=1;

    for(int i=0;i<N_SCREENS;i++){
        const ScreenSeed *sc=&SCREENS[i];
        for(int row=1;row<=sc->rows;row++){
            char lbl[4]={0};
            if(row<=26){ lbl[0]=(char)('A'+row-1); }
            else{ int idx=row-27; lbl[0]=(char)('A'+idx/26); lbl[1]=(char)('A'+idx%26); }
            int recliner_start = sc->rows - sc->rec_end + 1;
            int premium_start  = sc->rows - sc->rec_end - sc->prem_end + 1;
            int stype = (sc->rec_end > 0 && row >= recliner_start) ? 2 :
                        (sc->prem_end > 0 && row >= premium_start)  ? 1 : 0;
            for(int col=1;col<=sc->cols;col++){
                uint8_t *r=table_add(&tSeats);
                put_i32(r, 0,(int32_t)seat_id_ctr);
                put_i32(r, 4,(int32_t)sc->id);
                put_str(r, 8, 3,lbl);
                put_i32(r,11,(int32_t)col);
                put_i32(r,15,(int32_t)stype);
                put_i32(r,19,1);
                seat_id_ctr++;
            }
        }
    }
    int total_seats=seat_id_ctr-1;
    table_flush(&tSeats); table_free(&tSeats);
    printf("  -> %d total seats across %d screens\n",total_seats,N_SCREENS);

    /* ── [11] shows (placeholder empty page) ───────────────────────────── */
    printf("\n[11/18] shows (placeholder)\n");
    write_empty_page_file("shows.db");
    {
        Index ixShowsDT = index_new("shows_datetime.idx", 1);
        Index ixShowsMv = index_new("shows_movieid.idx", 1);
        index_flush(&ixShowsDT, RS_SHOWS);
        index_flush(&ixShowsMv, RS_SHOWS);
        index_free(&ixShowsDT);
        index_free(&ixShowsMv);
    }

    /* ── [12] seat_status (placeholder empty page) ─────────────────────── */
    printf("\n[12/18] seat_status (placeholder)\n");
    write_empty_page_file("seat_status.db");
    {
        Index ixSSShow = index_new("seat_status_showid.idx", 1);
        index_flush(&ixSSShow, RS_SEAT_STATUS);
        index_free(&ixSSShow);
    }

    /* ── [13] bookings (empty) ──────────────────────────────────────────── */
    printf("\n[13/18] bookings (empty)\n");
    Table tBookings=table_new("bookings.db",RS_BOOKINGS);
    Index ixBkUser =index_new("bookings_userid.idx",1);
    table_flush(&tBookings); index_flush(&ixBkUser,RS_BOOKINGS);
    table_free(&tBookings); index_free(&ixBkUser);

    /* ── [14] booking_seats (empty) ─────────────────────────────────────── */
    printf("\n[14/18] booking_seats (empty)\n");
    Table tBS=table_new("booking_seats.db",RS_BOOKING_SEATS);
    table_flush(&tBS); table_free(&tBS);

    /* ── [15] payments (empty) ──────────────────────────────────────────── */
    printf("\n[15/18] payments (empty)\n");
    Table tPay=table_new("payments.db",RS_PAYMENTS);
    table_flush(&tPay); table_free(&tPay);

    /* ── [16] refunds (empty) ───────────────────────────────────────────── */
    printf("\n[16/18] refunds (empty)\n");
    Table tRef=table_new("refunds.db",RS_REFUNDS);
    table_flush(&tRef); table_free(&tRef);

    /* ── [17] promos ─────────────────────────────────────────────────────── */
    printf("\n[17/18] promos\n");
    Table tPromos=table_new("promos.db",RS_PROMOS);
    {
        uint8_t *r;

        r=table_add(&tPromos);
        put_i32(r,0,1); put_str(r,4,20,"WELCOME10"); put_i32(r,24,0); put_f32(r,28,10.0f);
        put_f32(r,32,100.0f); put_i32(r,36,1); put_i32(r,40,7); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-12-31"); put_i32_null(r,84); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,2); put_str(r,4,20,"STUDENT50"); put_i32(r,24,1); put_f32(r,28,50.0f);
        put_f32_null(r,32); put_i32(r,36,1); put_i32(r,40,2); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-12-31"); put_i32_null(r,84); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,3); put_str(r,4,20,"GROUP15"); put_i32(r,24,0); put_f32(r,28,15.0f);
        put_f32(r,32,300.0f); put_i32(r,36,4); put_i32(r,40,7); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-12-31"); put_i32(r,84,500); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,4); put_str(r,4,20,"WEEKEND75"); put_i32(r,24,1); put_f32(r,28,75.0f);
        put_f32_null(r,32); put_i32(r,36,1); put_i32(r,40,3); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-06-30"); put_i32_null(r,84); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,5); put_str(r,4,20,"ADMIN25"); put_i32(r,24,0); put_f32(r,28,25.0f);
        put_f32_null(r,32); put_i32(r,36,1); put_i32(r,40,4); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-12-31"); put_i32(r,84,100); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,6); put_str(r,4,20,"HOLI100"); put_i32(r,24,1); put_f32(r,28,100.0f);
        put_f32_null(r,32); put_i32(r,36,2); put_i32(r,40,7); put_date(r,44,"2026-03-13");
        put_date(r,64,"2026-03-15"); put_i32(r,84,200); put_i32(r,88,0); put_i32(r,92,1);

        r=table_add(&tPromos);
        put_i32(r,0,7); put_str(r,4,20,"NEWUSER20"); put_i32(r,24,0); put_f32(r,28,20.0f);
        put_f32(r,32,200.0f); put_i32(r,36,1); put_i32(r,40,7); put_date(r,44,"2026-01-01");
        put_date(r,64,"2026-12-31"); put_i32_null(r,84); put_i32(r,88,0); put_i32(r,92,1);
    }
    table_flush(&tPromos); table_free(&tPromos);

    /* ── [18] waitlist (empty) ───────────────────────────────────────────── */
    printf("\n[18/18] waitlist (empty)\n");
    Table tWait=table_new("waitlist.db",RS_WAITLIST);
    table_flush(&tWait); table_free(&tWait);

    /* ── WAL header ──────────────────────────────────────────────────────── */
    printf("\n[+] wal.log (empty)\n");
    {
        FILE *f=fopen("data/wal.log","wb"); if(!f) die("data/wal.log");
        fclose(f);
        printf("  wrote wal.log  (0 transactions)\n");
    }

    printf("\n============================================================\n");
    printf("Seed complete.\n");
    printf("  Countries     : 1\n");
    printf("  Cities        : %d\n",n_cities);
    printf("  Acad. domains : %d\n",n_domains);
    printf("  Refund tiers  : 4\n");
    printf("  Users         : %d  (1 admin, 2 students, %d regular)\n",n_users,n_users-3);
    printf("  Movies        : 0\n");
    printf("  Cast entries  : 0\n");
    printf("  Theatres      : %d  (7 cities)\n",N_THEATRES);
    printf("  Screens       : %d\n",N_SCREENS);
    printf("  Seats         : %d\n",total_seats);
    printf("  Shows         : 0\n");
    printf("  Seat-status   : 0\n");
    printf("  Promos        : %d\n",7);
    printf("\nRun the app as Admin → Movie Management → Super Import to populate movies and auto-schedule shows.\n");
    printf("Admin login — phone: 9000000001  password: admin123\n");
    return 0;
}