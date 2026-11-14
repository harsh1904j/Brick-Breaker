#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <conio.h>
  #include <windows.h>
  #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
  #endif
#else
  #include <termios.h>
  #include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

const int W = 40;
const int H = 24;
const int COLS = 10;
const int ROWS = 10;
const int BW = 4;
const int TOP = 2;
const int PR = H - 2;
const int MAX_LEVEL = 100;

static std::mt19937 rng{std::random_device{}()};
static int randInt(int n) { return (int)(rng() % (unsigned)n); }

template <class T>
static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

#ifndef _WIN32
static termios g_orig;
static bool g_raw = false;
void enableRaw() {
    if (g_raw) return;
    tcgetattr(STDIN_FILENO, &g_orig);
    termios t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = true;
}
void disableRaw() {
    if (!g_raw) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
    g_raw = false;
}
void flushInput() { tcflush(STDIN_FILENO, TCIFLUSH); }
#else
void enableRaw() {}
void disableRaw() {}
void flushInput() { while (_kbhit()) _getch(); }
#endif

int pollKey() {
#ifdef _WIN32
    if (!_kbhit()) return 0;
    int c = _getch();
    if (c == 0 || c == 224) {
        int d = _getch();
        if (d == 75) return 'a';
        if (d == 77) return 'd';
        return 0;
    }
    return std::tolower(c);
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == 27) {
        unsigned char s0 = 0, s1 = 0;
        if (read(STDIN_FILENO, &s0, 1) == 1 && read(STDIN_FILENO, &s1, 1) == 1 && s0 == '[') {
            if (s1 == 'D') return 'a';
            if (s1 == 'C') return 'd';
        }
        return 0;
    }
    return std::tolower(c);
#endif
}

void sleepMs(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static bool g_sound = true;
static int g_sndPrio = 0, g_sndFreq = 0, g_sndMs = 0;

void soundNow(int freq, int ms) {
    if (!g_sound) return;
#ifdef _WIN32
    Beep((DWORD)freq, (DWORD)ms);
#else
    (void)freq; (void)ms;
    std::fputc('\a', stdout);
    std::fflush(stdout);
#endif
}

void queueSound(int prio, int freq, int ms) {
    if (prio > g_sndPrio) { g_sndPrio = prio; g_sndFreq = freq; g_sndMs = ms; }
}

void flushSound() {
#ifdef _WIN32
    if (g_sndPrio > 0) soundNow(g_sndFreq, g_sndMs);
#else
    if (g_sndPrio >= 3) soundNow(g_sndFreq, g_sndMs);
#endif
    g_sndPrio = 0;
}

int waitForKey(const std::string& accepted) {
    enableRaw();
    flushInput();
    for (;;) {
        int k = pollKey();
        if (k && accepted.find((char)k) != std::string::npos) {
            disableRaw();
            return k;
        }
        sleepMs(20);
    }
}

void clearScreen() { std::fputs("\033[2J\033[H", stdout); std::fflush(stdout); }

void restoreTerminal() {
    disableRaw();
    std::fputs("\033[0m\033[?25h", stdout);
    std::fflush(stdout);
}

void setupTerminal() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    std::atexit(restoreTerminal);
    std::signal(SIGINT, [](int) { std::exit(0); });
}

struct Ball { float x, y, vx, vy; };
struct Token { float x, y; int type; };

enum TokenType { T_MULT, T_MULTIBALL, T_REDUCE, T_LIFE, T_WIDE,
                 T_SHRINK, T_FAST, T_REVERSE, T_COUNT };
const char TOKEN_GLYPH[T_COUNT] = {'M', 'B', 'R', 'L', 'W', 'S', 'F', 'X'};
const int  TOKEN_COLOR[T_COUNT] = {93, 96, 91, 92, 95, 1, 1, 1};
inline bool isBadToken(int t) { return t >= T_SHRINK; }

const char* DIFF_NAMES[3] = {"Easy", "Normal", "Hard"};
inline int   startLives(int d) { return d == 0 ? 5 : d == 1 ? 3 : 2; }
inline float diffSpeed(int d)  { return d == 0 ? 0.8f : d == 1 ? 1.0f : 1.2f; }

struct Game {
    std::string name;
    int age = 0;
    int grid[ROWS][COLS] = {};
    int bricksLeft = 0;
    int level = 1;
    int lives = 3;
    int score = 0;
    std::vector<Ball> balls;
    std::vector<Token> tokens;
    int brokenCount = 0;
    int multTimer = 0;
    int fastTimer = 0;
    int reverseTimer = 0;
    int diff = 1;
    float speed = 0.4f;
    int px = 0, pw = 10;
    bool launched = false;
    std::string msg;
    int msgTimer = 0;
    int roastCooldown = 0;
};

enum Result { CLEARED, LOST, QUIT };

static std::string exeDir() {
    std::string p;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) p.assign(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) p.assign(buf, (size_t)n);
#endif
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}
static std::string savePath() { return exeDir() + "brickbreaker.sav"; }
static std::string scorePath() { return exeDir() + "brickbreaker.hs"; }

void saveGame(const Game& g) {
    std::ofstream f(savePath());
    if (f) f << g.name << "\n" << g.age << " " << g.level << " " << g.score << " " << g.lives << " " << g.diff << "\n";
}

bool loadGame(Game& g) {
    std::ifstream f(savePath());
    if (!f) return false;
    std::string name;
    int age, level, score, lives;
    if (!std::getline(f, name)) return false;
    if (!(f >> age >> level >> score >> lives)) return false;
    int diff = 1;
    if (!(f >> diff) || diff < 0 || diff > 2) diff = 1;
    if (name.empty() || age < 1 || age > 120 || level < 1 || level > MAX_LEVEL || lives < 1) return false;
    g.name = name; g.age = age; g.level = level; g.score = score; g.lives = std::min(lives, 9);
    g.diff = diff;
    return true;
}

void deleteSave() { std::remove(savePath().c_str()); }

struct HS { std::string name; int score, level, diff; };

std::vector<HS> loadScores() {
    std::vector<HS> v;
    std::ifstream f(scorePath());
    HS h;
    while (f >> h.score >> h.level >> h.diff) {
        f.get();
        if (!std::getline(f, h.name)) break;
        if (h.diff < 0 || h.diff > 2) h.diff = 1;
        v.push_back(h);
    }
    return v;
}

int addScore(const Game& g) {
    if (g.score <= 0) return -1;
    std::vector<HS> v = loadScores();
    v.push_back(HS{g.name, g.score, g.level, g.diff});
    std::stable_sort(v.begin(), v.end(), [](const HS& a, const HS& b) { return a.score > b.score; });
    int rank = -1;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].name == g.name && v[i].score == g.score && v[i].level == g.level && v[i].diff == g.diff) { rank = (int)i; break; }
    if (v.size() > 5) v.resize(5);
    std::ofstream f(scorePath());
    for (const HS& h : v) f << h.score << " " << h.level << " " << h.diff << " " << h.name << "\n";
    return rank < 5 ? rank : -1;
}

void printScores(int highlight = -1) {
    std::vector<HS> v = loadScores();
    if (v.empty()) return;
    std::cout << "\033[1;36m  ------- HIGH SCORES -------\033[0m\n";
    for (size_t i = 0; i < v.size() && i < 5; i++) {
        char line[128];
        std::snprintf(line, sizeof line, "  %d. %-15s %7d   Lv %-3d %-6s", (int)i + 1,
                      v[i].name.c_str(), v[i].score, v[i].level, DIFF_NAMES[v[i].diff]);
        std::cout << line << ((int)i == highlight ? "  \033[1;32m<- you!\033[0m" : "") << "\n";
    }
    std::cout << "\n";
}

std::string fmt(std::string s, const Game& g) {
    auto rep = [&](const std::string& key, const std::string& val) {
        size_t p;
        while ((p = s.find(key)) != std::string::npos) s.replace(p, key.size(), val);
    };
    rep("{name}", g.name);
    rep("{age}", std::to_string(g.age));
    return s;
}

template <class T>
const T& pick(const std::vector<T>& v) { return v[randInt((int)v.size())]; }

const std::vector<std::string> BREAK_ROASTS = {
    "Wow {name}, a brick! Want a medal or a nap?",
    "Nice one, {name}. My grandma's screensaver plays better.",
    "Careful {name}, don't strain your one working brain cell.",
    "Was that skill, or did the ball just feel sorry for you?",
    "{name} broke a brick. Scientists are baffled.",
    "Ooh, a brick. Put that on your resume, {name}.",
    "Even a broken clock gets lucky, {name}.",
    "Your paddle is held together by hope and bad aim, {name}.",
    "Keep going {name}, the bricks are shaking. With laughter.",
    "Is that your best, {name}? Cute.",
    "Bricks: one less. Your dignity: also one less.",
    "Did you do that on purpose, {name}? Be honest.",
    "My calculator plays this better than you, {name}.",
    "Lucky bounce. Don't let it go to your head, {name}.",
    "A brick falls. {name} celebrates. Everyone else yawns.",
    "Impressive... for someone who needs both hands.",
    "Slow clap for {name}. Very slow. Like your reflexes.",
    "Your ball has more sense of direction than you, {name}.",
    "That brick wasn't even trying, {name}.",
    "Breaking bricks is easy. Breaking the habit of being bad is harder.",
};

std::vector<std::string> ageRoasts(int age) {
    if (age < 13) return {
        "Only {age}? Go finish your homework, {name}. The bricks will wait.",
        "At {age}, {name}, your thumbs should be faster than this.",
    };
    if (age < 20) return {
        "At {age} you should have gamer reflexes, {name}. Where are they?",
        "{age} and still can't aim. School must be so proud, {name}.",
    };
    if (age < 40) return {
        "{age} years of life experience and THIS is the result, {name}?",
        "At {age}, {name}, you should be paying taxes, not missing bricks.",
    };
    if (age < 60) return {
        "At {age}, {name}, this is peak hobby energy. Sadly, not peak skill.",
        "{age} years old and the paddle is still faster than you, {name}.",
    };
    return {
        "{age} and still gaming, {name}? Respect. Also, your aim is terrible.",
        "At {age}, {name}, you've seen it all. Except a ball you can catch.",
    };
}

const std::vector<std::string> LIFE_TAUNTS = {
    "HA! The ball left you, {name}. Even it has standards.",
    "Missed! Did you blink, {name}?",
    "That ball just ghosted you, {name}.",
    "{name}, the paddle is the wide thing at the bottom. USE IT.",
    "Oof. And {name} was doing so well. (Not really.)",
    "The ball said bye. Relatable, {name}.",
    "Gravity: 1. {name}: 0.",
    "Is your monitor even on, {name}?",
    "At {age} you'd think you'd catch a ball by now, {name}.",
    "I'd say 'nice try' but that would be lying, {name}.",
    "The ball fell faster than your self-esteem, {name}.",
    "Butterfingers! Well, butter-paddle.",
};

const std::vector<std::string> GAMEOVER_TAUNTS = {
    "GAME OVER, {name}! The bricks send their regards. And laughter.",
    "{name} has been defeated by rectangles. Let that sink in.",
    "Out of lives, {name}! Maybe try checkers. The slow kind.",
    "Game over. Even the bricks feel bad for you, {name}. Almost.",
    "At {age}, losing to bricks. Bold career choice, {name}.",
    "{name}, I've seen better aim from a sleepy cat.",
    "The bricks are throwing a party in your honor, {name}. You're not invited.",
};

const std::vector<std::string> CLEAR_ROASTS = {
    "Level cleared, {name}. Don't get cocky. You're still bad.",
    "You beat it, {name}! The bricks were probably just tired.",
    "Cleared! Took you long enough, {name}.",
    "Fine, {name}, that was acceptable. Barely.",
    "Level done! Your mom would be proud, I guess. Maybe.",
    "Another wall down. Still not impressed, {name}.",
    "You cleared it, {name}. The bricks were on easy mode. Probably.",
    "Look at you go, {name}. Slowly. Painfully. But go.",
    "Congratulations, {name}! That's the participation trophy of Brick Breaker.",
    "Good job. Now do it again, but with some style, {name}.",
};

std::string breakRoast(const Game& g) {
    if (randInt(100) < 30) return fmt(pick(ageRoasts(g.age)), g);
    return fmt(pick(BREAK_ROASTS), g);
}

const char* PATTERN_NAMES[10] = {
    "Full Wall", "Checkerboard", "Pyramid", "Upside-Down Pyramid", "Stripes",
    "Diamond", "Pillars", "Fortress", "Zigzag Highway", "BOSS: Chaos Scatter"};
const char* TIER_NAMES[10] = {
    "Warm-Up", "Rookie", "Sneaky", "Stubborn", "Spicy",
    "Nasty", "Brutal", "Savage", "Insane", "LEGENDARY"};

std::string levelTitle(int level) {
    int tier = (level - 1) / 10, pat = (level - 1) % 10;
    return "Level " + std::to_string(level) + " - " + TIER_NAMES[tier] + " " + PATTERN_NAMES[pat];
}

static bool scatterHash(int r, int c, int L) {
    unsigned h = (unsigned)r * 73856093u ^ (unsigned)c * 19349663u ^ (unsigned)L * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h % 100 < 62;
}

static bool patternFilled(int pat, int r, int c, int R, int L) {
    switch (pat) {
        case 0: return true;
        case 1: return (r + c) % 2 == 0;
        case 2: return std::fabs(c - 4.5) < (r + 1) * 5.0 / R;
        case 3: return std::fabs(c - 4.5) < (R - r) * 5.0 / R;
        case 4: return r % 2 == 0;
        case 5: return std::fabs(c - 4.5) / 4.5 + std::fabs(r - (R - 1) / 2.0) / (R / 2.0) <= 1.0;
        case 6: return c % 3 != 1;
        case 7: return r == 0 || r == R - 1 || c == 0 || c == COLS - 1 ||
                       (r >= 2 && r < R - 2 && c >= 3 && c <= 6);
        case 8: return (c + r) % 4 < 2;
        default: return scatterHash(r, c, L);
    }
}

void buildLevel(Game& g) {
    int L = g.level;
    int tier = (L - 1) / 10, pat = (L - 1) % 10;
    int R = std::min(ROWS, 5 + tier / 2 + ((pat == 2 || pat == 3 || pat == 5 || pat == 7) ? 1 : 0));

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g.grid[r][c] = 0;
    g.bricksLeft = 0;

    for (int r = 0; r < R; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!patternFilled(pat, r, c, R, L)) continue;
            int hp = 1;
            if (tier >= 2 && (r + c) % 3 == 0) hp++;
            if (tier >= 5 && (r + 2 * c) % 4 == 0) hp++;
            if (tier >= 3 && r == 0) hp++;
            hp = std::min(hp, 3);
            int mod = (tier >= 6) ? 8 : 11;
            if (tier >= 3 && (r * 7 + c * 3 + L) % mod == 0) hp = -1;
            g.grid[r][c] = hp;
            if (hp > 0) g.bricksLeft++;
        }
    }
    if (g.bricksLeft == 0) {
        for (int c = 0; c < COLS; c++) { g.grid[0][c] = 1; g.bricksLeft++; }
    }

    g.speed = (0.55f + 0.005f * L) * diffSpeed(g.diff);
    g.pw = std::max(5, 10 - tier / 2);
    g.px = (W - g.pw) / 2;
}

void resetBall(Game& g) {
    g.launched = false;
    g.balls.assign(1, Ball{g.px + g.pw / 2.0f, PR - 0.5f, 0.0f, 0.0f});
    g.tokens.clear();
    g.fastTimer = 0;
    g.reverseTimer = 0;
}

bool brickCell(const Game& g, int ix, int iy, int& r, int& c) {
    r = iy - TOP;
    c = ix / BW;
    if (ix < 0 || ix >= W || r < 0 || r >= ROWS || c < 0 || c >= COLS) return false;
    return g.grid[r][c] != 0;
}

int scoreMult(const Game& g) { return g.multTimer > 0 ? 2 : 1; }
float curSpeed(const Game& g) { return g.speed * (g.fastTimer > 0 ? 1.35f : 1.0f); }

void spawnToken(Game& g, int r, int c) {
    int roll = randInt(100), type;
    if (roll < 22) type = T_MULT;
    else if (roll < 40) type = T_MULTIBALL;
    else if (roll < 55) type = T_REDUCE;
    else if (roll < 62) type = T_LIFE;
    else if (roll < 72) type = T_WIDE;
    else if (roll < 82) type = T_SHRINK;
    else if (roll < 91) type = T_FAST;
    else type = T_REVERSE;
    g.tokens.push_back(Token{(float)(c * BW + BW / 2), (float)(r + TOP), type});
}

void applyToken(Game& g, int type) {
    switch (type) {
        case T_MULT:
            g.multTimer = 400;
            g.msg = "TOKEN: Score x2 for 10 seconds!";
            break;
        case T_MULTIBALL: {
            std::vector<Ball> extra;
            for (const Ball& b : g.balls)
                for (int i = 0; i < 4; i++) {
                    float dir = (i % 2 ? 1.0f : -1.0f) * (0.3f + 0.35f * (i / 2 + 1) + 0.1f * randInt(3));
                    extra.push_back(Ball{b.x, b.y, dir * curSpeed(g), -curSpeed(g)});
                }
            for (const Ball& b : extra)
                if (g.balls.size() < 15) g.balls.push_back(b);
            g.msg = "TOKEN: Balls x5!";
            break;
        }
        case T_REDUCE: {
            std::vector<std::pair<int, int>> cells;
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    if (g.grid[r][c] > 0) cells.push_back({r, c});
            std::shuffle(cells.begin(), cells.end(), rng);
            int n = std::min<int>(10, (int)cells.size());
            for (int i = 0; i < n; i++) {
                g.grid[cells[i].first][cells[i].second] = 0;
                g.bricksLeft--;
                g.score += 10 * scoreMult(g);
            }
            g.msg = "TOKEN: " + std::to_string(n) + " bricks destroyed!";
            break;
        }
        case T_LIFE:
            if (g.lives < 9) g.lives++;
            g.msg = "TOKEN: Extra life!";
            break;
        case T_WIDE:
            g.pw = std::min(16, g.pw + 4);
            g.px = std::min(g.px, W - g.pw);
            g.msg = "TOKEN: Wider paddle!";
            break;
        case T_SHRINK:
            g.pw = std::max(4, g.pw - 3);
            g.msg = "BAD TOKEN: Paddle shrunk!";
            break;
        case T_FAST:
            if (g.fastTimer == 0)
                for (Ball& b : g.balls) { b.vx *= 1.35f; b.vy *= 1.35f; }
            g.fastTimer = 300;
            g.msg = "BAD TOKEN: Balls faster for 8 seconds!";
            break;
        case T_REVERSE:
            g.reverseTimer = 300;
            g.msg = "BAD TOKEN: Controls reversed for 8 seconds!";
            break;
    }
    if (isBadToken(type)) queueSound(3, 220, 100);
    else queueSound(3, 1400, 60);
    g.msgTimer = 70;
    g.roastCooldown = 45;
}

void updateTokens(Game& g) {
    if (g.multTimer > 0) g.multTimer--;
    if (g.fastTimer > 0) g.fastTimer--;
    if (g.reverseTimer > 0) g.reverseTimer--;
    for (size_t i = 0; i < g.tokens.size();) {
        Token& t = g.tokens[i];
        t.y += 0.25f;
        bool remove = false;
        if ((int)t.y >= PR && (int)t.y <= PR && t.x >= g.px - 1.0f && t.x < g.px + g.pw + 1.0f) {
            applyToken(g, t.type);
            remove = true;
        } else if ((int)t.y > PR) {
            remove = true;
        }
        if (remove) g.tokens.erase(g.tokens.begin() + i);
        else i++;
    }
}

void hitBrick(Game& g, int r, int c) {
    int& h = g.grid[r][c];
    if (h < 0) return;
    h--;
    g.score += 5 * scoreMult(g);
    queueSound(h == 0 ? 2 : 1, h == 0 ? 1000 : 700, h == 0 ? 20 : 12);
    if (h == 0) {
        g.bricksLeft--;
        g.score += 10 * scoreMult(g);
        g.brokenCount++;
        if (g.brokenCount % 5 == 0) spawnToken(g, r, c);
        if (g.roastCooldown == 0 && g.bricksLeft > 0) {
            g.msg = breakRoast(g);
            g.msgTimer = 80;
            g.roastCooldown = 45;
        }
    }
}

bool moveOne(Game& g, Ball& b) {
    float m = std::max(std::fabs(b.vx), std::fabs(b.vy));
    int n = std::max(1, (int)std::ceil(m / 0.35f));

    for (int i = 0; i < n; i++) {
        float ox = b.x;
        b.x += b.vx / n;
        if (b.x < 0) { b.x = std::min(-b.x, (float)W - 0.01f); b.vx = -b.vx; }
        else if (b.x >= W) { b.x = std::max(0.0f, 2.0f * W - 0.02f - b.x); b.vx = -b.vx; }
        int r, c;
        if (brickCell(g, (int)b.x, (int)b.y, r, c)) {
            b.x = ox;
            b.vx = -b.vx;
            hitBrick(g, r, c);
        }

        float oy = b.y;
        b.y += b.vy / n;
        if (b.y < 0) { b.y = -b.y; b.vy = -b.vy; }
        if (brickCell(g, (int)b.x, (int)b.y, r, c)) {
            b.y = oy;
            b.vy = -b.vy;
            hitBrick(g, r, c);
        }

        if (b.vy > 0 && (int)b.y == PR && b.x >= g.px - 0.5f && b.x < g.px + g.pw + 0.5f) {
            b.y = PR - 0.01f;
            queueSound(1, 450, 12);
            float rel = ((b.x - g.px) / g.pw) * 2.0f - 1.0f;
            rel = clampv(rel, -1.0f, 1.0f);
            float sp = curSpeed(g);
            b.vx = rel * sp * 1.3f;
            if (std::fabs(b.vx) < 0.05f) b.vx = (randInt(2) ? 1 : -1) * 0.08f;
            b.vy = -sp * (1.0f - 0.35f * std::fabs(rel));
        }

        if (b.y >= PR + 1) return true;
    }
    return false;
}

bool moveBall(Game& g) {
    for (size_t i = 0; i < g.balls.size();) {
        if (moveOne(g, g.balls[i])) g.balls.erase(g.balls.begin() + i);
        else i++;
    }
    return g.balls.empty();
}

void render(const Game& g, bool paused) {
    static const int rowCol[4] = {96, 92, 94, 95};
    static char cell[H][W];
    static int col[H][W];

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { cell[y][x] = ' '; col[y][x] = 0; }

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int hp = g.grid[r][c];
            if (!hp) continue;
            int color = hp < 0 ? 37 : hp == 3 ? 91 : hp == 2 ? 93 : rowCol[r % 4];
            for (int k = 0; k < BW; k++) {
                int x = c * BW + k, y = r + TOP;
                cell[y][x] = (k == 0) ? '[' : (k == BW - 1) ? ']' : (hp < 0 ? 'X' : '#');
                col[y][x] = color;
            }
        }
    }
    for (int x = g.px; x < g.px + g.pw; x++) { cell[PR][x] = '='; col[PR][x] = 96; }
    for (const Token& t : g.tokens) {
        int tx = clampv((int)t.x, 1, W - 2), ty = clampv((int)t.y, 0, H - 1);
        cell[ty][tx - 1] = isBadToken(t.type) ? '<' : '(';
        cell[ty][tx] = TOKEN_GLYPH[t.type];
        cell[ty][tx + 1] = isBadToken(t.type) ? '>' : ')';
        for (int k = -1; k <= 1; k++) col[ty][tx + k] = TOKEN_COLOR[t.type];
    }
    for (const Ball& b : g.balls) {
        int ix = clampv((int)b.x, 0, W - 1), iy = clampv((int)b.y, 0, H - 1);
        cell[iy][ix] = 'O';
        col[iy][ix] = 97;
    }

    std::string out = "\033[H";
    std::string fx;
    if (g.multTimer > 0) fx += "  \033[1;93mx2 " + std::to_string(g.multTimer / 38 + 1) + "s\033[0m";
    if (g.fastTimer > 0) fx += "  \033[1;91mFAST\033[0m";
    if (g.reverseTimer > 0) fx += "  \033[1;91mREVERSED\033[0m";
    out += "\033[1;33m  " + levelTitle(g.level) + "\033[0m" + fx + "\033[K\n";
    out += "  " + g.name + " (" + std::to_string(g.age) + ")   Lives: " + std::to_string(g.lives) +
           "   Score: " + std::to_string(g.score) + "   Bricks: " + std::to_string(g.bricksLeft) +
           "\033[K\n";
    out += "+" + std::string(W, '-') + "+\033[K\n";
    for (int y = 0; y < H; y++) {
        out += "|";
        int cur = 0;
        for (int x = 0; x < W; x++) {
            if (col[y][x] != cur) {
                cur = col[y][x];
                out += cur ? ("\033[1;" + (cur == 1 ? std::string("97;41") : std::to_string(cur)) + "m") : "\033[0m";
            }
            out += cell[y][x];
        }
        out += "\033[0m|\033[K\n";
    }
    out += "+" + std::string(W, '-') + "+\033[K\n";

    if (paused) out += "\033[1;36m PAUSED - press P to continue\033[0m\033[K\n";
    else if (g.msgTimer > 0) out += "\033[1;35m " + g.msg + "\033[0m\033[K\n";
    else if (!g.launched) out += "\033[1;32m Press SPACE to launch the ball!\033[0m\033[K\n";
    else out += "\033[K\n";
    out += "\033[90m A/D or arrows: move | SPACE: launch | P: pause | Q: quit | M: sound\033[0m\033[K\n";

    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

Result playLevel(Game& g) {
    saveGame(g);
    buildLevel(g);
    resetBall(g);
    g.msg.clear();
    g.msgTimer = 0;
    g.roastCooldown = 0;
    g.brokenCount = 0;
    g.multTimer = 0;

    clearScreen();
    enableRaw();
    flushInput();
    bool paused = false;
    auto next = std::chrono::steady_clock::now();

    for (;;) {
        int k;
        while ((k = pollKey()) != 0) {
            if (k == 'q') { disableRaw(); return QUIT; }
            if (k == 'm') { g_sound = !g_sound; continue; }
            if (k == 'p') paused = !paused;
            if (paused) continue;
            if (g.reverseTimer > 0 && (k == 'a' || k == 'd')) k = (k == 'a') ? 'd' : 'a';
            if (k == 'a') g.px = std::max(0, g.px - 4);
            if (k == 'd') g.px = std::min(W - g.pw, g.px + 4);
            if (k == ' ' && !g.launched) {
                g.launched = true;
                g.balls[0].vx = (randInt(2) ? 1 : -1) * g.speed * 0.5f;
                g.balls[0].vy = -g.speed;
            }
        }

        if (!paused) {
            if (g.msgTimer > 0) g.msgTimer--;
            if (g.roastCooldown > 0) g.roastCooldown--;

            if (!g.launched) {
                g.balls[0].x = g.px + g.pw / 2.0f;
                g.balls[0].y = PR - 0.5f;
            } else if (moveBall(g)) {
                g.lives--;
                queueSound(4, 160, 150);
                if (g.lives <= 0) { render(g, false); disableRaw(); return LOST; }
                g.msg = fmt(pick(LIFE_TAUNTS), g);
                g.msgTimer = 100;
                resetBall(g);
            } else {
                updateTokens(g);
            }
        }

        render(g, paused);
        flushSound();

        if (g.bricksLeft <= 0) { disableRaw(); return CLEARED; }

        next += std::chrono::milliseconds(26);
        auto now = std::chrono::steady_clock::now();
        if (next > now)
            sleepMs((int)std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());
        else
            next = now;
    }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void askPlayer(Game& g) {
    clearScreen();
    std::cout << "\033[1;33m";
    std::cout << "  ===============================\n";
    std::cout << "        B R I C K  B R E A K E R\n";
    std::cout << "          100 levels. No mercy.\n";
    std::cout << "  ===============================\033[0m\n\n";
    printScores();

    std::string name;
    std::cout << "  Enter your username: ";
    std::getline(std::cin, name);
    name = trim(name);
    if (name.size() > 15) name = name.substr(0, 15);
    if (name.empty()) name = "Mystery Noob";
    g.name = name;

    for (;;) {
        std::string line;
        std::cout << "  Enter your age: ";
        if (!std::getline(std::cin, line)) { g.age = 18; break; }
        try {
            int a = std::stoi(trim(line));
            if (a >= 1 && a <= 120) { g.age = a; break; }
        } catch (...) {}
        std::cout << "  Nice try. A real age, please.\n";
    }

    std::cout << "\n  Choose difficulty:\n";
    std::cout << "    1) Easy    (5 lives, slower ball)\n";
    std::cout << "    2) Normal  (3 lives)\n";
    std::cout << "    3) Hard    (2 lives, faster ball)\n";
    std::cout << "  Press 1, 2 or 3: ";
    std::cout.flush();
    g.diff = waitForKey("123") - '1';
    g.lives = startLives(g.diff);
    std::cout << DIFF_NAMES[g.diff] << "\n";
}

int main() {
    setupTerminal();
    std::fputs("\033[?25l", stdout);

    Game g;
    bool resumed = false;
    if (loadGame(g)) {
        clearScreen();
        std::cout << "\033[1;33m  ===============================\n";
        std::cout << "        B R I C K  B R E A K E R\n";
        std::cout << "  ===============================\033[0m\n\n";
        std::cout << "  Saved game found for \033[1;36m" << g.name << "\033[0m (age " << g.age << "):\n";
        std::cout << "    " << levelTitle(g.level) << "\n";
        std::cout << "    Score: " << g.score << "   Lives: " << g.lives << "   Difficulty: " << DIFF_NAMES[g.diff] << "\n\n";
        printScores();
        std::cout << "  Press C to continue, N for a new game, or Q to quit.\n";
        int k = waitForKey("cnq");
        if (k == 'q') return 0;
        if (k == 'c') resumed = true;
        else g = Game();
    }

    if (!resumed) {
        askPlayer(g);

        std::cout << "\n\033[1;35m  Welcome, " << g.name << "! " << g.age
                  << "? Oh... interesting. Let's see how badly this goes.\033[0m\n";
        std::cout << "\n  Break every brick to clear a level. Clear all 100 levels to win.\n";
        std::cout << "  Grey [XX] bricks are unbreakable. Red/yellow bricks take several hits.\n";
        std::cout << "  Every 5th brick you break drops a token. Catch it with the paddle:\n";
        std::cout << "    (M) score x2 10s  (B) balls x5  (R) destroy 10 bricks\n";
        std::cout << "    (L) extra life    (W) wider paddle\n";
        std::cout << "    BAD: <S> shrink paddle  <F> faster balls  <X> reversed keys\n";
        std::cout << "  Press M any time to toggle sound.\n";
        std::cout << "  I will be commenting on your performance. Constantly.\n";
        std::cout << "\n  Press SPACE to start (or Q to quit).\n";
        if (waitForKey(" q") == 'q') return 0;
    }

    for (;;) {
        Result r = playLevel(g);

        if (r == QUIT) {
            clearScreen();
            std::cout << fmt("\n  Quitting already, {name}? Classic. The bricks win by default.\n\n", g);
            std::cout << "  Progress saved. You'll resume at " << levelTitle(g.level) << ".\n\n";
            break;
        }

        if (r == CLEARED) {
            g.score += 100 + g.lives * 20;
            soundNow(900, 70); soundNow(1200, 70); soundNow(1500, 110);

            if (g.level >= MAX_LEVEL) {
                clearScreen();
                std::cout << "\033[1;32m\n  *** ALL 100 LEVELS CLEARED! ***\033[0m\n\n";
                deleteSave();
                int rank = addScore(g);
                std::cout << fmt("  Alright {name}, every single wall is destroyed. The roasting stops here.\n", g);
                std::cout << fmt("  You're officially a Brick Legend at {age}. (I'm secretly impressed.)\n", g);
                std::cout << "\n  Final score: " << g.score << "\n\n";
                printScores(rank);
                break;
            }

            clearScreen();
            std::cout << "\033[1;32m\n  LEVEL " << g.level << " CLEARED!\033[0m\n\n";
            std::cout << "\033[1;35m  " << fmt(pick(CLEAR_ROASTS), g) << "\033[0m\n\n";
            if (g.level % 10 == 0 && g.lives < 9) {
                g.lives++;
                std::cout << "  Boss beaten! +1 life (don't waste it).\n";
            }
            std::cout << "  Score: " << g.score << "   Lives: " << g.lives << "\n\n";
            std::cout << "  Next up: " << levelTitle(g.level + 1) << "\n";
            std::cout << "  Press SPACE to continue (Q to quit).\n";
            if (waitForKey(" q") == 'q') {
                clearScreen();
                g.level++;
                saveGame(g);
                std::cout << fmt("\n  Quitting already, {name}? Classic.\n\n", g);
                std::cout << "  Progress saved. You'll resume at " << levelTitle(g.level) << ".\n\n";
                break;
            }
            g.level++;
        } else {
            clearScreen();
            {
                Game fresh = g;
                fresh.level = 1; fresh.score = 0; fresh.lives = startLives(g.diff);
                saveGame(fresh);
            }
            int rank = addScore(g);
            std::cout << "\033[1;31m\n  G A M E   O V E R\033[0m\n\n";
            std::cout << "\033[1;35m  " << fmt(pick(GAMEOVER_TAUNTS), g) << "\033[0m\n\n";
            std::cout << "  You died on " << levelTitle(g.level) << " with " << g.score << " points.\n\n";
            printScores(rank);
            std::cout << "  Press R to restart from Level 1 with " << startLives(g.diff) << " lives, or Q to quit.\n";
            if (waitForKey("rq") == 'q') {
                std::cout << fmt("\n  Running away, {name}? Smart. The bricks were winning anyway.\n\n", g);
                break;
            }
            g.lives = startLives(g.diff);
            g.level = 1;
            g.score = 0;
        }
    }
    return 0;
}