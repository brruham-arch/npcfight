#include <jni.h>
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

#define TAG "NPCFight"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ============================================================
// OFFSETS — libGTASA.so armeabi-v7a (versi ini)
// ============================================================

// Semua offset ini adalah offset dari base libGTASA.so
// Diambil dari hasil nm -D libGTASA.so

#define OFF_FindPlayerPed           0x0040b288  // FindPlayerPed(int)
#define OFF_CPopulation_AddPed      0x004cf26c  // CPopulation::AddPed(ePedType, modelId, CVector&, bool)
#define OFF_CPed_GiveWeapon         0x0049f518  // CPed::GiveWeapon(eWeaponType, ammo, bool)
#define OFF_CTaskManager_SetTask    0x0053390a  // CTaskManager::SetTask(CTask*, int, bool)
#define OFF_CGame_Process           0x003f3fb0  // CGame::Process()

// Task constructors
#define OFF_TaskKillPedOnFoot_ctor      0x004e01b0  // CTaskComplexKillPedOnFoot(CPed*, i,j,j,j,i)
#define OFF_TaskKillPedOnFootArmed_ctor 0x004e2520  // CTaskComplexKillPedOnFootArmed(CPed*, j,j,j,i)
#define OFF_TaskKillPedOnFootMelee_ctor 0x004e17cc  // CTaskComplexKillPedOnFootMelee(CPed*)
#define OFF_TaskSimpleFight_ctor        0x004d86b0  // CTaskSimpleFight(CEntity*, i, j)

// CPedIntelligence
#define OFF_CPedIntelligence_ClearTasks 0x004c08ec  // CPedIntelligence::ClearTasks(bool, bool)

// Struct offsets (dari re3 Android, perlu verifikasi)
#define OFFSET_PED_INTELLIGENCE     0x47C   // CPed::m_pIntelligence
#define OFFSET_PED_POSITION         0x14    // CPed/CPhysical::m_placement -> CVector pos

// Weapon IDs (eWeaponType)
#define WEAPON_FIST         0
#define WEAPON_PISTOL       22
#define WEAPON_SHOTGUN      25
#define WEAPON_AK47         30
#define WEAPON_M4           31
#define WEAPON_KNIFE        4

// Ped model IDs
#define MODEL_COP       265
#define MODEL_SWAT      267
#define MODEL_ARMY      287
#define MODEL_BALLAS    102
#define MODEL_GROVE     105
#define MODEL_VAGOS     114

// ============================================================
// TYPEDEFS — function pointer ke fungsi libGTASA
// ============================================================

typedef void* (*FindPlayerPed_t)(int playerIndex);
typedef void* (*CPopulation_AddPed_t)(int pedType, int modelId, float* pos, bool unknown);
typedef void  (*CPed_GiveWeapon_t)(void* ped, int weaponType, int ammo, bool unknown);
typedef void  (*CTaskManager_SetTask_t)(void* taskMgr, void* task, int slot, bool forceNewTask);
typedef void  (*CGame_Process_t)();
typedef void* (*TaskKillPedOnFootMelee_ctor_t)(void* task, void* targetPed);
typedef void* (*TaskKillPedOnFootArmed_ctor_t)(void* task, void* targetPed, unsigned int flags1, unsigned int flags2, unsigned int flags3, int unknown);
typedef void  (*CPedIntelligence_ClearTasks_t)(void* intel, bool bForceRestart, bool bClearScriptTask);

// ============================================================
// GLOBAL STATE
// ============================================================

static uintptr_t g_gtasaBase = 0;
static bool g_initialized = false;
static bool g_hooked = false;

// Daftar NPC yang sudah di-spawn
// Simpan sebagai raw pointer — perlu validasi sebelum akses
struct NPCEntry {
    void* ped;
    int   modelId;
    int   weaponId;
};

static std::vector<NPCEntry> g_npcList;
static pthread_mutex_t g_npcMutex = PTHREAD_MUTEX_INITIALIZER;

// Counter frame untuk polling (tidak tiap frame agar tidak berat)
static int g_frameCounter = 0;
static int g_spawnRequest = 0;  // > 0 berarti ada request spawn
static int g_spawnModel   = MODEL_COP;
static int g_spawnWeapon  = WEAPON_AK47;

// Dobby hook — original CGame::Process
static CGame_Process_t g_origCGameProcess = nullptr;

// ============================================================
// HELPER: Dapat fungsi pointer dari offset
// ============================================================

template<typename T>
static T getFunc(uintptr_t offset) {
    // ARM Thumb: bit0 = 1 untuk Thumb mode
    // Fungsi di .text section ini Thumb, jadi tambah +1
    return (T)(g_gtasaBase + offset + 1);
}

// ============================================================
// HELPER: Dapat pointer ke CTaskManager dari CPed
// ============================================================

static void* getTaskManager(void* ped) {
    if (!ped) return nullptr;
    // CPed::m_pIntelligence ada di offset 0x47C
    void* intel = *(void**)((uintptr_t)ped + OFFSET_PED_INTELLIGENCE);
    if (!intel) return nullptr;
    // CTaskManager ada di awal CPedIntelligence (offset 0x0)
    return intel;  // m_TaskMgr adalah member pertama
}

// ============================================================
// HELPER: Dapat posisi CPed
// ============================================================

static void getPedPosition(void* ped, float* x, float* y, float* z) {
    if (!ped) { *x = *y = *z = 0; return; }
    float* pos = (float*)((uintptr_t)ped + OFFSET_PED_POSITION);
    *x = pos[0];
    *y = pos[1];
    *z = pos[2];
}

// ============================================================
// HELPER: Cek apakah ped masih hidup (health > 0)
// Health ada di CPed::m_fHealth — offset dari re3: 0x540
// ============================================================

#define OFFSET_PED_HEALTH   0x540

static float getPedHealth(void* ped) {
    if (!ped) return 0.0f;
    return *(float*)((uintptr_t)ped + OFFSET_PED_HEALTH);
}

// ============================================================
// CORE: Assign task "bunuh target" ke ped
// ============================================================

static void assignKillTask(void* attacker, void* target) {
    if (!attacker || !target) return;

    void* taskMgr = getTaskManager(attacker);
    if (!taskMgr) return;

    // Alokasi task object di heap
    // Size CTaskComplexKillPedOnFootArmed dari re3 ~= 0x44 bytes
    void* taskMem = malloc(0x80);
    if (!taskMem) return;
    memset(taskMem, 0, 0x80);

    // Construct task
    auto taskCtor = getFunc<TaskKillPedOnFootArmed_ctor_t>(OFF_TaskKillPedOnFootArmed_ctor);
    taskCtor(taskMem, target, 0, 0, 0, 0);

    // Assign ke slot PRIMARY (slot 0)
    auto setTask = getFunc<CTaskManager_SetTask_t>(OFF_CTaskManager_SetTask);
    setTask(taskMgr, taskMem, 0, true);

    LOGI("Assigned kill task: attacker=%p -> target=%p", attacker, target);
}

// ============================================================
// CORE: Spawn satu NPC di sekitar player
// ============================================================

static void spawnNPC(int modelId, int weaponId) {
    auto findPlayer = getFunc<FindPlayerPed_t>(OFF_FindPlayerPed);
    void* playerPed = findPlayer(0);
    if (!playerPed) {
        LOGE("spawnNPC: playerPed null");
        return;
    }

    float px, py, pz;
    getPedPosition(playerPed, &px, &py, &pz);

    // Random offset 4-7 meter dari player
    float angle = ((float)(rand() % 360)) * 3.14159f / 180.0f;
    float dist  = 4.0f + (float)(rand() % 3);
    float spawnPos[3] = {
        px + cosf(angle) * dist,
        py + sinf(angle) * dist,
        pz
    };

    // Spawn ped
    // CPopulation::AddPed(ePedType=4 (CIVMALE), modelId, pos, false)
    auto addPed = getFunc<CPopulation_AddPed_t>(OFF_CPopulation_AddPed);
    void* newPed = addPed(4, modelId, spawnPos, false);

    if (!newPed) {
        LOGE("spawnNPC: AddPed returned null");
        return;
    }

    LOGI("Spawned NPC: ped=%p model=%d", newPed, modelId);

    // Kasih senjata
    auto giveWeapon = getFunc<CPed_GiveWeapon_t>(OFF_CPed_GiveWeapon);
    giveWeapon(newPed, weaponId, 300, false);

    pthread_mutex_lock(&g_npcMutex);

    // NPC baru menyerang semua NPC lama
    for (auto& entry : g_npcList) {
        if (getPedHealth(entry.ped) > 0.0f) {
            assignKillTask(newPed, entry.ped);
            // NPC lama juga menyerang NPC baru
            assignKillTask(entry.ped, newPed);
            // Cukup assign ke target pertama yang hidup untuk sekarang
            // Retarget logic ada di monitor
            break;
        }
    }

    // Masuk list
    NPCEntry entry;
    entry.ped      = newPed;
    entry.modelId  = modelId;
    entry.weaponId = weaponId;
    g_npcList.push_back(entry);

    pthread_mutex_unlock(&g_npcMutex);
}

// ============================================================
// CORE: Monitor NPC — cek mati, retarget
// Dipanggil setiap ~60 frame (~1 detik)
// ============================================================

static void monitorNPCs() {
    pthread_mutex_lock(&g_npcMutex);

    // Hapus NPC yang sudah mati dari list
    for (int i = (int)g_npcList.size() - 1; i >= 0; i--) {
        if (getPedHealth(g_npcList[i].ped) <= 0.0f) {
            LOGI("NPC mati: ped=%p, hapus dari list", g_npcList[i].ped);
            g_npcList.erase(g_npcList.begin() + i);
        }
    }

    // Retarget: setiap NPC yang hidup assign attack ke NPC lain yang hidup
    // Sederhana: NPC[i] attack NPC[i+1], NPC terakhir attack NPC[0]
    int count = (int)g_npcList.size();
    if (count >= 2) {
        for (int i = 0; i < count; i++) {
            int targetIdx = (i + 1) % count;
            void* attacker = g_npcList[i].ped;
            void* target   = g_npcList[targetIdx].ped;
            if (attacker && target) {
                assignKillTask(attacker, target);
            }
        }
    }

    pthread_mutex_unlock(&g_npcMutex);
}

// ============================================================
// TRIGGER: Baca file command
// Format file /sdcard/npcfight.txt:
//   "spawn"        → spawn NPC dengan model/weapon default
//   "spawn 267"    → spawn model 267 (SWAT)
//   "spawn 267 30" → spawn model 267, weapon 30 (AK47)
//   "clear"        → hapus semua NPC dari list (tidak kill)
// ============================================================

static void checkFileCommand() {
    const char* cmdFile = "/sdcard/npcfight.txt";
    FILE* f = fopen(cmdFile, "r");
    if (!f) return;

    char line[64] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);

    // Hapus file setelah dibaca agar tidak trigger ulang
    remove(cmdFile);

    // Parse command
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
        // Parse optional: "spawn MODEL WEAPON"
        sscanf(line, "spawn %d %d", &model, &weapon);
        spawnNPC(model, weapon);
    }
}

// ============================================================
// HOOK: CGame::Process — dipanggil tiap frame
// ============================================================

static void hookedCGameProcess() {
    // Panggil original dulu
    if (g_origCGameProcess) {
        g_origCGameProcess();
    }

    g_frameCounter++;

    // Cek file command setiap 30 frame (~0.5 detik)
    if (g_frameCounter % 30 == 0) {
        checkFileCommand();
    }

    // Monitor NPC setiap 60 frame (~1 detik)
    if (g_frameCounter % 60 == 0) {
        monitorNPCs();
    }

    // Reset counter agar tidak overflow
    if (g_frameCounter >= 3600) {
        g_frameCounter = 0;
    }
}

// ============================================================
// INIT: Cari base address libGTASA.so
// ============================================================

static uintptr_t getLibraryBase(const char* libName) {
    char line[512];
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libName) && strstr(line, "r-xp")) {
            base = (uintptr_t)strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

// ============================================================
// HOOK: Pasang hook ke CGame::Process via inline hook sederhana
// Kita pakai pendekatan thumb trampoline manual
// (Dobby tidak tersedia di sini, pakai manual patch)
// ============================================================

// Untuk Thumb function: kita perlu 8-byte trampoline
// LDR PC, [PC, #0]  (Thumb-2: 32-bit)
// .word target_address

static uint8_t g_origBytes[8];

static bool hookCGameProcess() {
    uintptr_t funcAddr = g_gtasaBase + OFF_CGame_Process;
    // Thumb: addr & ~1, tapi kita akses memory actual tanpa +1
    uintptr_t patchAddr = funcAddr; // OFF sudah tanpa +1

    // Simpan original bytes
    memcpy(g_origBytes, (void*)patchAddr, 8);

    // Buat trampoline untuk original (6 bytes pertama + jump back)
    // Alokasi trampoline
    uint8_t* tramp = (uint8_t*)memalign(4, 16);
    memcpy(tramp, g_origBytes, 8);

    // Jump back ke patchAddr+8
    uintptr_t retAddr = patchAddr + 8;
    // LDR PC, [PC, #0] dalam Thumb-2 = F8 DF F0 00
    tramp[8]  = 0xDF; tramp[9]  = 0xF8;
    tramp[10] = 0xF0; tramp[11] = 0x00;
    *(uintptr_t*)(tramp + 12) = retAddr | 1; // +1 untuk Thumb

    g_origCGameProcess = (CGame_Process_t)((uintptr_t)tramp | 1);

    // Patch fungsi original dengan jump ke hooked
    // mprotect dulu agar bisa tulis
    uintptr_t pageStart = patchAddr & ~(4095);
    mprotect((void*)pageStart, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);

    uintptr_t hookAddr = (uintptr_t)hookedCGameProcess | 1; // Thumb
    uint8_t patch[8];
    patch[0] = 0xDF; patch[1] = 0xF8; // LDR.W PC, [PC, #0]
    patch[2] = 0xF0; patch[3] = 0x00;
    *(uintptr_t*)(patch + 4) = hookAddr;

    memcpy((void*)patchAddr, patch, 8);

    // Cache flush
    __builtin___clear_cache((char*)patchAddr, (char*)patchAddr + 8);
    __builtin___clear_cache((char*)tramp, (char*)tramp + 16);

    LOGI("Hook CGame::Process dipasang di 0x%x", (unsigned)patchAddr);
    return true;
}

// ============================================================
// ENTRY POINT — dipanggil AML
// ============================================================

extern "C" void OnModLoad() {
    LOGI("NPCFight OnModLoad dipanggil");

    // Cari base libGTASA.so
    g_gtasaBase = getLibraryBase("libGTASA.so");
    if (!g_gtasaBase) {
        LOGE("Gagal dapat base libGTASA.so");
        return;
    }
    LOGI("libGTASA.so base: 0x%x", (unsigned)g_gtasaBase);

    srand(12345);

    // Pasang hook
    if (hookCGameProcess()) {
        g_hooked = true;
        g_initialized = true;
        LOGI("NPCFight siap. Tulis ke /sdcard/npcfight.txt untuk spawn NPC.");
        LOGI("Format: 'spawn [modelId] [weaponId]' atau 'clear'");
    } else {
        LOGE("Hook gagal");
    }
}

// AML mod info
struct ModInfo {
    const char* id;
    const char* name;
    const char* version;
    const char* author;
};

extern "C" ModInfo* __GetModInfo() {
    static ModInfo info = {
        "com.brruham.npcfight",
        "NPC Fight",
        "1.0",
        "brruham"
    };
    return &info;
}

extern "C" const char* __INeedASpecificGame() {
    return "com.rockstargames.gtasa";
}
