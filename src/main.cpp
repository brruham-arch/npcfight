#include <mod/amlmod.h>
#include <mod/logger.h>

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <stdint.h>
#include <sys/mman.h>

// ============================================================
// AML mod registration — WAJIB pakai macro ini, bukan struct manual
// MYMOD otomatis export __GetModInfo() dengan layout yang benar
// ============================================================
MYMOD(com.brruham.npcfight, NPC Fight, 1.0, brruham)
NEEDGAME(com.rockstargames.gtasa)

// ============================================================
// FILE LOGGER
// ============================================================

static const char* LOG_FILE =
    "/storage/emulated/0/Android_unprotected/data/"
    "com.rockstargames.gtasa/files/npcfight_log.txt";
static int g_logLine = 0;

static void fileLog(const char* level, const char* fmt, ...) {
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(f, "[%s][%d] %s\n", level, g_logLine++, buf);
    fflush(f);
    fclose(f);
}

static void logClear() {
    remove(LOG_FILE);
    FILE* f = fopen(LOG_FILE, "w");
    if (f) { fprintf(f, "=== NPCFight Log Start ===\n"); fclose(f); }
}

#define LOGI(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_INFO,  "NPCFight", fmt, ##__VA_ARGS__); \
    fileLog("I", fmt, ##__VA_ARGS__); \
} while(0)

#define LOGE(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_ERROR, "NPCFight", fmt, ##__VA_ARGS__); \
    fileLog("E", fmt, ##__VA_ARGS__); \
} while(0)

// ============================================================
// OFFSETS — libGTASA.so armeabi-v7a
// ============================================================

#define OFF_FindPlayerPed               0x0040b288
#define OFF_CPopulation_AddPed          0x004cf26c
#define OFF_CPed_GiveWeapon             0x0049f518
#define OFF_CTaskManager_SetTask        0x0053390a
#define OFF_CGame_Process               0x003f3fb0
#define OFF_TaskKillPedOnFootArmed_ctor 0x004e2520
#define OFF_CPedIntelligence_ClearTasks 0x004c08ec

#define OFFSET_PED_INTELLIGENCE     0x47C
#define OFFSET_PED_POSITION         0x14
#define OFFSET_PED_HEALTH           0x540

#define WEAPON_PISTOL   22
#define WEAPON_SHOTGUN  25
#define WEAPON_AK47     30
#define WEAPON_M4       31

#define MODEL_COP       265
#define MODEL_SWAT      267
#define MODEL_ARMY      287
#define MODEL_BALLAS    102
#define MODEL_GROVE     105

// ============================================================
// TYPEDEFS
// ============================================================

typedef void* (*FindPlayerPed_t)(int);
typedef void* (*CPopulation_AddPed_t)(int, int, float*, bool);
typedef void  (*CPed_GiveWeapon_t)(void*, int, int, bool);
typedef void  (*CTaskManager_SetTask_t)(void*, void*, int, bool);
typedef void  (*CGame_Process_t)();
typedef void* (*TaskKillPedOnFootArmed_ctor_t)(void*, void*, unsigned, unsigned, unsigned, int);

// ============================================================
// GLOBALS
// ============================================================

static uintptr_t g_gtasaBase = 0;
static bool      g_initialized = false;

struct NPCEntry { void* ped; int modelId; int weaponId; };
static std::vector<NPCEntry> g_npcList;
static pthread_mutex_t       g_npcMutex = PTHREAD_MUTEX_INITIALIZER;

static int g_frameCounter = 0;
static int g_spawnModel   = MODEL_COP;
static int g_spawnWeapon  = WEAPON_AK47;

// Original CGame::Process — diisi oleh aml->Hook()
static CGame_Process_t g_origCGameProcess = nullptr;

// ============================================================
// HELPER: function pointer dari offset (Thumb: +1)
// ============================================================

template<typename T>
static inline T getFunc(uintptr_t offset) {
    return (T)(g_gtasaBase + offset + 1);
}

// ============================================================
// HELPER: baca posisi ped
// ============================================================

static void getPedPosition(void* ped, float* x, float* y, float* z) {
    if (!ped) { *x = *y = *z = 0; return; }
    float* pos = (float*)((uintptr_t)ped + OFFSET_PED_POSITION);
    *x = pos[0]; *y = pos[1]; *z = pos[2];
}

// ============================================================
// HELPER: baca health ped
// ============================================================

static inline float getPedHealth(void* ped) {
    if (!ped) return 0.0f;
    return *(float*)((uintptr_t)ped + OFFSET_PED_HEALTH);
}

// ============================================================
// HELPER: ambil TaskManager dari ped
// ============================================================

static void* getTaskManager(void* ped) {
    if (!ped) return nullptr;
    void* intel = *(void**)((uintptr_t)ped + OFFSET_PED_INTELLIGENCE);
    if (!intel) return nullptr;
    return intel; // CTaskManager adalah member pertama CPedIntelligence
}

// ============================================================
// CORE: assign task bunuh target ke attacker
// ============================================================

static void assignKillTask(void* attacker, void* target) {
    if (!attacker || !target) return;

    void* taskMgr = getTaskManager(attacker);
    if (!taskMgr) {
        LOGE("assignKillTask: taskMgr null — OFFSET_PED_INTELLIGENCE=0x%x mungkin salah",
             OFFSET_PED_INTELLIGENCE);
        return;
    }

    // Alokasi dan construct task object
    void* taskMem = malloc(0x80);
    if (!taskMem) { LOGE("assignKillTask: malloc gagal"); return; }
    memset(taskMem, 0, 0x80);

    auto taskCtor = getFunc<TaskKillPedOnFootArmed_ctor_t>(OFF_TaskKillPedOnFootArmed_ctor);
    taskCtor(taskMem, target, 0, 0, 0, 0);

    auto setTask = getFunc<CTaskManager_SetTask_t>(OFF_CTaskManager_SetTask);
    setTask(taskMgr, taskMem, 0, true);
}

// ============================================================
// CORE: spawn satu NPC di sekitar player
// ============================================================

static void spawnNPC(int modelId, int weaponId) {
    LOGI("spawnNPC: model=%d weapon=%d", modelId, weaponId);

    auto findPlayer = getFunc<FindPlayerPed_t>(OFF_FindPlayerPed);
    void* playerPed = findPlayer(0);
    if (!playerPed) { LOGE("spawnNPC: playerPed null"); return; }

    float px, py, pz;
    getPedPosition(playerPed, &px, &py, &pz);

    float angle   = ((float)(rand() % 360)) * 3.14159f / 180.0f;
    float dist    = 4.0f + (float)(rand() % 3);
    float pos[3]  = { px + cosf(angle) * dist, py + sinf(angle) * dist, pz };

    LOGI("spawnNPC: player=(%.1f,%.1f,%.1f) spawn=(%.1f,%.1f,%.1f)",
         px, py, pz, pos[0], pos[1], pos[2]);

    auto addPed = getFunc<CPopulation_AddPed_t>(OFF_CPopulation_AddPed);
    void* newPed = addPed(4, modelId, pos, false);
    if (!newPed) { LOGE("spawnNPC: AddPed null — model invalid atau pool penuh"); return; }
    LOGI("spawnNPC: newPed=0x%08x", (unsigned)(uintptr_t)newPed);

    auto giveWeapon = getFunc<CPed_GiveWeapon_t>(OFF_CPed_GiveWeapon);
    giveWeapon(newPed, weaponId, 300, false);

    pthread_mutex_lock(&g_npcMutex);

    // NPC baru <-> NPC lama saling serang
    for (auto& e : g_npcList) {
        if (getPedHealth(e.ped) > 0.0f) {
            assignKillTask(newPed, e.ped);
            assignKillTask(e.ped, newPed);
            break;
        }
    }

    g_npcList.push_back({ newPed, modelId, weaponId });
    pthread_mutex_unlock(&g_npcMutex);
    LOGI("spawnNPC: selesai, total NPC=%d", (int)g_npcList.size());
}

// ============================================================
// CORE: monitor — hapus NPC mati, retarget yang hidup
// ============================================================

static void monitorNPCs() {
    pthread_mutex_lock(&g_npcMutex);

    // Hapus yang mati
    for (int i = (int)g_npcList.size() - 1; i >= 0; i--) {
        if (getPedHealth(g_npcList[i].ped) <= 0.0f) {
            LOGI("NPC mati, hapus index=%d", i);
            g_npcList.erase(g_npcList.begin() + i);
        }
    }

    // Retarget ring: NPC[i] attack NPC[i+1]
    int count = (int)g_npcList.size();
    if (count >= 2) {
        for (int i = 0; i < count; i++) {
            assignKillTask(g_npcList[i].ped, g_npcList[(i + 1) % count].ped);
        }
    }

    pthread_mutex_unlock(&g_npcMutex);
}

// ============================================================
// TRIGGER: baca file command
// Format: "spawn [modelId] [weaponId]" atau "clear"
// ============================================================

static const char* CMD_FILE =
    "/storage/emulated/0/Android_unprotected/data/"
    "com.rockstargames.gtasa/files/npcfight.txt";

static void checkFileCommand() {
    FILE* f = fopen(CMD_FILE, "r");
    if (!f) return;
    char line[64] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);
    remove(CMD_FILE);
    LOGI("command: '%s'", line);

    if (strncmp(line, "clear", 5) == 0) {
        pthread_mutex_lock(&g_npcMutex);
        g_npcList.clear();
        pthread_mutex_unlock(&g_npcMutex);
        LOGI("NPC list cleared");
        return;
    }
    if (strncmp(line, "spawn", 5) == 0) {
        int model  = g_spawnModel;
        int weapon = g_spawnWeapon;
        sscanf(line, "spawn %d %d", &model, &weapon);
        spawnNPC(model, weapon);
    }
}

// ============================================================
// HOOK: CGame::Process — tiap frame
// ============================================================

static void hookedCGameProcess() {
    if (g_origCGameProcess) g_origCGameProcess();

    g_frameCounter++;

    if (g_frameCounter % 30  == 0) checkFileCommand();
    if (g_frameCounter % 60  == 0) monitorNPCs();
    if (g_frameCounter >= 3600)    g_frameCounter = 0;
}

// ============================================================
// ENTRY POINT
// ============================================================

extern "C" void OnModLoad() {
    logClear();
    LOGI("OnModLoad dipanggil — build %s %s", __DATE__, __TIME__);

    // aml sudah tersedia dari macro MYMOD via GetInterface("AMLInterface")
    if (!aml) {
        LOGE("aml interface null — AML tidak ter-load dengan benar");
        return;
    }

    // Dapatkan base libGTASA.so via AML (lebih reliable dari /proc/self/maps)
    g_gtasaBase = aml->GetLib("libGTASA.so");
    if (!g_gtasaBase) {
        LOGE("GAGAL: base libGTASA.so tidak ditemukan");
        return;
    }
    LOGI("libGTASA.so base: 0x%08x", (unsigned)g_gtasaBase);
    LOGI("CGame::Process:   0x%08x", (unsigned)(g_gtasaBase + OFF_CGame_Process));
    LOGI("FindPlayerPed:    0x%08x", (unsigned)(g_gtasaBase + OFF_FindPlayerPed));
    LOGI("AddPed:           0x%08x", (unsigned)(g_gtasaBase + OFF_CPopulation_AddPed));

    srand(12345);

    // Hook CGame::Process via AML — handle Thumb, mprotect, cache flush otomatis
    // Thumb function: pass addr+1 agar AML/GlossHook tahu ini Thumb
    uintptr_t processAddr = g_gtasaBase + OFF_CGame_Process + 1;
    bool ok = aml->Hook(
        (void*)processAddr,
        (void*)hookedCGameProcess,
        (void**)&g_origCGameProcess
    );

    if (!ok || !g_origCGameProcess) {
        LOGE("Hook CGame::Process GAGAL");
        return;
    }

    g_initialized = true;
    LOGI("Hook OK — origFn=0x%08x", (unsigned)(uintptr_t)g_origCGameProcess);
    LOGI("NPCFight siap. Tulis npcfight.txt untuk spawn:");
    LOGI("  'spawn'          → COP + AK47");
    LOGI("  'spawn 267 25'   → SWAT + Shotgun");
    LOGI("  'spawn 287 31'   → Army + M4");
    LOGI("  'clear'          → reset list NPC");
}
