# 🎬 CineBook — Terminal Cinema Booking System

<div align="center">

```
   ██████╗██╗███╗   ██╗███████╗██████╗  ██████╗  ██████╗ ██╗  ██╗
  ██╔════╝██║████╗  ██║██╔════╝██╔══██╗██╔═══██╗██╔═══██╗██║ ██╔╝
  ██║     ██║██╔██╗ ██║█████╗  ██████╔╝██║   ██║██║   ██║█████╔╝ 
  ██║     ██║██║╚██╗██║██╔══╝  ██╔══██╗██║   ██║██║   ██║██╔═██╗ 
  ╚██████╗██║██║ ╚████║███████╗██████╔╝╚██████╔╝╚██████╔╝██║  ██╗
   ╚═════╝╚═╝╚═╝  ╚═══╝╚══════╝╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝
```

**A feature-complete terminal cinema booking platform built from scratch in C/C++ — no SQLite, no Django, no shortcuts.**

[![Language](https://img.shields.io/badge/C11%20%2F%20C%2B%2B17-primary-blue?style=flat-square&logo=c)](#)
[![Engine](https://img.shields.io/badge/Database-Custom%20RDBMS-brightgreen?style=flat-square)](#)
[![TMDB](https://img.shields.io/badge/TMDB-Live%20Movie%20Data-01d277?style=flat-square)](#)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey?style=flat-square)](#)
[![License](https://img.shields.io/badge/License-MIT-purple?style=flat-square)](#)

*Your Cinema. Your Seats. Your Story.*

</div>

---

## What is CineBook?

CineBook is a fully functional, terminal-based cinema booking system.
It simulates a real-world booking platform with:

- **Live movie data** pulled from [TMDB](https://www.themoviedb.org/) via libcurl
- A **custom RDBMS engine** written from scratch — 4 KB page files, WAL crash recovery, LRU buffer pool, hash + sorted indexes
- A **C++ OOP analytics layer** with abstract report classes, polymorphic dispatch, and STL containers
- A **secure first-run setup flow** with encrypted TMDB key storage (`.api_key`) and setup marker (`.setup_complete`)
- A rich **ANSI terminal UI** — interactive seat maps, price breakdowns, booking receipts

No SQLite. No PostgreSQL. No Python. Every byte on disk is managed by the C program files.

---

## ⚡ One-Command Setup

The fastest way to get CineBook running is the platform launcher script — it installs missing dependencies, builds, seeds the database, and drops you straight into the app.

### Windows (MSYS2 required)

> **First:** Install [MSYS2](https://www.msys2.org/), then open **MSYS2 MinGW 64-bit** terminal.

```bash
git clone https://github.com/R1sh1m/CineBook.git
cd CineBook
bash run.sh
```

Or from CMD / PowerShell:

```batch
git clone https://github.com/R1sh1m/CineBook.git
cd CineBook
run.bat
```

### macOS

```bash
git clone https://github.com/R1sh1m/CineBook.git
cd CineBook
bash run.sh
```

### Linux (Ubuntu / Debian / Fedora)

```bash
git clone https://github.com/R1sh1m/CineBook.git
cd CineBook
bash run.sh
```

The script will automatically install `libcurl` if missing, build the project, seed the database on first run, and launch CineBook.

---

## 🛠️ Manual Setup

If you prefer to control each step yourself:

### Step 1 — Install Prerequisites

<details>
<summary><strong>Windows (MSYS2 MinGW 64-bit terminal)</strong></summary>

```bash
pacman -S --needed \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-curl-openssl \
  make git
```

</details>

<details>
<summary><strong>macOS</strong></summary>

```bash
# Install Homebrew if you don't have it
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

brew install gcc curl
```

</details>

<details>
<summary><strong>Ubuntu / Debian</strong></summary>

```bash
sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev git
```

</details>

<details>
<summary><strong>Fedora / RHEL</strong></summary>

```bash
sudo dnf install -y gcc gcc-c++ make libcurl-devel git
```

</details>

---

### Step 2 — Build

```bash
make
```

This compiles all `.c` files with `gcc -Wall -std=c11` and `reports.cpp` with `g++ -Wall -std=c++17`, then links with `g++`.

### Step 3 — Seed the Database

Run this **once** before your first launch. It creates the binary `.db` and `.idx` files in `data/`.

```bash
gcc -std=c11 -Wall -O2 -o seed tools/seed.c
./seed
```

This generates:
- 10 cities across India
- 17 theatres, 38 screens (~6,010 seats)
- 8 seeded movies with cast
- 114 shows across all screens
- 7 promo codes and 20 academic domains

### Step 4 — Run

```bash
make run
# or
./cinebook
```

---

##  Demo Accounts

| Name | Phone | Password | Role |
|---|---|---|---|
| Admin CineBook | `9000000001` | `admin123` | 🔑 Admin |
| Arjun Sharma | `9876543210` | `pass1234` | 👤 User |
| Priya Nair | `9845123456` | `pass1234` | 🎓 Student |
| Rahul Mehta | `9123456789` | `cinema99` | 👤 User |
| Sneha Patel | `9988776655` | `student1` | 🎓 Student |
| Karthik Rajan | `9090909090` | `pass1234` | 👤 User |
| Meera Krishnan | `8877665544` | `student1` | 🎓 Student |

Students automatically get a **12% seat discount**. Accounts with an academic email domain (`.ac.in`, `.edu`) are detected at sign-up and promoted.

---

## 🔑 TMDB API Key (Optional)

CineBook works out of the box with the seeded dataset. To enable **live movie imports** from The Movie Database:

1. Create a free account at [themoviedb.org/settings/api](https://www.themoviedb.org/settings/api)
2. Generate a **v3 API key**
3. Launch CineBook and complete the first-run wizard prompt  
   (Admin can later update via **Movie Management → Update TMDB API key (secure wizard)**)

Notes:
- API keys are stored encrypted in `.api_key`
- Setup completion is tracked by `.setup_complete`
- Legacy plaintext `TMDB_API_KEY` from config is migrated automatically when possible

Log in as Admin → **Movie Management** → **SUPER IMPORT** to pull currently showing titles (India region), or use **Import from TMDB** to search by title.

Without a key, manual movie entry remains available via Admin → Movie Management → Add Movie Manually.

---

## 🗺️ User Flow

```
Sign Up / Login
│
├── Browse Movies      — filter by genre, language, city
│       └── View Detail   — synopsis, full cast, streaming platforms
│               └── Select Show   — city-filtered, future only
│                       └── Interactive Seat Map   — ANSI grid
│                               └── Price Breakdown   — 12-slot itemised receipt
│                                       └── Apply Promo Code   (optional)
│                                               └── Pay
│                                                   ├── Wallet (instant)
│                                                   ├── UPI   (VPA validation)
│                                                   ├── Card  (Luhn check + expiry)
│                                                   └── Net Banking (10 banks)
│                                                           └── ✓ Booking Confirmed
│
├── My Upcoming Bookings
├── My Past Bookings
├── Cancel a Booking   — tiered refund (100% / 75% / 50% / 0%)
├── Add Funds to Wallet
├── View Notifications — waitlist alerts when seats free up
└── Account Settings   — change city, password, add email / student upgrade
```

### Admin Flow

```
Admin Login
│
├── Movie Management   — TMDB import, Super Import (bulk now-playing), manual add
├── Theatre Management — add/edit theatres, deactivate seats
├── Screen Management  — create screens with layout (2D / IMAX / 4DX)
├── Show Management    — schedule shows, conflict detection, bulk cancel with refund
├── Promo Management   — create/deactivate promo codes (% or flat, role-masked)
├── Analytics Dashboard
│       ├── Occupancy Report   — fill rate per show, ANSI bar chart
│       ├── Revenue Report     — by movie and by theatre
│       └── Booking Trends     — by hour-of-day and day-of-week
│               └── Export to CSV  →  exports/dashboard_YYYYMMDD_HHMMSS.csv
└── System Management  — cities, academic domains, refund policy tiers, integrity tools, database optimize
```

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  ① Entry        main.c  ·  cinebook.conf  ·  setup wizard   │
├──────────────────────────────────────────────────────────────┤
│  ② Bootstrap    schema.c  ·  storage.c  ·  txn.c  ·  index.c │
├──────────────────────────────────────────────────────────────┤
│  ③ Auth         auth.c  ·  session.c                        │
│                 SHA-256 (pure C)  ·  SessionContext*         │
├──────────────────────────────────────────────────────────────┤
│  ④ UI Layer     ui_browse  ·  ui_booking  ·  ui_cancel       │
│                 ui_account  ·  ui_admin  ·  ui_dashboard     │
├──────────────────────────────────────────────────────────────┤
│  ⑤ Business     pricing.c  ·  refund.c  ·  payment.c        │
│     Logic       promos.c  ·  location.c                      │
├──────────────────────────────────────────────────────────────┤
│  ⑥ C++ OOP      reports.cpp  (abstract Report base class)    │
│                 OccupancyReport  ·  RevenueReport            │
│                 BookingReport  ·  TMDBClient                  │
│                 ← extern "C" boundary via reports.h →        │
├──────────────────────────────────────────────────────────────┤
│  ⑦ RDBMS        query.c  ←→  schema.c  ·  storage.c         │
│     Engine      index.c  ·  txn.c  ·  record.c              │
├──────────────────────────────────────────────────────────────┤
│  ⑧ Data         *.db (binary pages)  ·  *.idx  ·  wal.log   │
└──────────────────────────────────────────────────────────────┘
```

**Key design rules:**
- `SessionContext*` is passed by pointer to every function — no global state
- `query.c` is the single gateway to all DB operations — nothing bypasses it
- The C/C++ boundary is **only** through `reports.h` with `extern "C"` guards
- Every write is WAL-logged before touching a page — crash recovery on startup

---

##  Technical Highlights

### Custom RDBMS Engine

| Component | Detail |
|-----------|--------|
| Page size | 4,096 bytes (8-byte header + 4,088 bytes data) |
| Buffer pool | 64-page LRU cache — dirty pages flushed on eviction |
| WAL | Full before/after page images, XOR checksum, auto-recovery at boot |
| Hash index | Open-address table — O(1) PK / email lookup |
| Sorted index | Binary search sorted array — O(log n) range queries |
| Record layout | Fixed byte-offset per `schema.cat`; NULL sentinels per type |
| Tables | 18 tables: users, movies, shows, seats, bookings, payments, refunds, promos… |

### Pricing Engine

Prices are computed fresh at every seat-hold from 12 slots:

```
base_price
  → + screen_surcharge   (IMAX +₹80–130, 4DX +₹200)
  → × seat_type_mult     (Standard ×1.0, Premium ×1.5, Recliner ×2.2)
  → subtotal_per_seat
  → − student_discount   (12% for verified academic accounts)
  → − group_discount     (8% for 6+ seats)
  → − promo_discount     (flat or %)
  → + dynamic_surge      (weekend +10%, peak hours 18:00–23:00 +15%, occupancy >70% +20%)
  → taxable_amount × seat_count
  → + GST 18%
  → + convenience_fee    (₹30 × seats)
  ══ grand_total
```

### C++ OOP Layer

- `Report` abstract base → `OccupancyReport`, `RevenueReport`, `BookingReport`
- `TMDBClient` encapsulates all HTTP calls (libcurl); throws `TMDBException : std::runtime_error`
- STL: `std::vector`, `std::map`, `std::sort`, `std::unique_ptr`
- Wrappers expose `run_report()`, `tmdb_search_and_import()`, `tmdb_bulk_import_now_playing()` to C callers

---

## 📂 Project Structure

```
CineBook/
├── main.c                   # Entry point — bootstrap, auth loop, role routing
├── Makefile                 # Cross-platform (Linux / macOS / MSYS2 Windows)
├── Cinebook.conf            # Runtime config (TMDB key, default city, currency)
├── run.sh                   # Auto-setup script (Linux / macOS / MSYS2)
├── run.bat                  # Auto-setup script (Windows CMD / PowerShell)
├── cinebook_dev.sh          # Developer helper (build / seed / run / test menu)
├── lib/
│   ├── cJSON.c / .h         # Lightweight JSON parser (Dave Gamble, MIT)
├── src/
│   ├── engine/
│   │   ├── storage.c / .h   # 4 KB page I/O, LRU buffer pool
│   │   ├── schema.c / .h    # schema.cat parser, in-memory catalogue
│   │   ├── record.c / .h    # Binary serialize / deserialize, ResultSet
│   │   ├── index.c / .h     # Hash + sorted in-memory indexes
│   │   ├── txn.c / .h       # Write-ahead log, crash recovery
│   │   └── query.c / .h     # Public RDBMS API (SELECT/INSERT/UPDATE/DELETE/JOIN)
│   ├── auth/
│   │   ├── auth.c / .h      # Login, signup, SHA-256, domain detection
│   │   └── session.c / .h   # SessionContext lifecycle
│   ├── logic/
│   │   ├── pricing.c / .h   # 12-slot price breakdown engine
│   │   ├── refund.c / .h    # Tiered refund + atomic cancellation
│   │   ├── payment.c / .h   # Wallet, UPI, Card, Net Banking simulation
│   │   ├── promos.c / .h    # Promo validation and application
│   │   └── location.c / .h  # City picker, theatre lookup, currency resolution
│   ├── reports/
│   │   ├── reports.cpp      # C++ OOP analytics + TMDB client
│   │   └── reports.h        # C-compatible extern "C" interface
│   └── ui/
│       ├── ui_browse.c      # Movie browsing, show listing
│       ├── ui_booking.c     # Interactive seat map, payment flow
│       ├── ui_cancel.c      # Cancellation UI, refund preview
│       ├── ui_account.c     # Account settings, student upgrade
│       ├── ui_admin.c       # Full admin panel
│       └── ui_dashboard.c   # Analytics dashboard, CSV export
├── tools/
│   └── seed.c               # One-shot database seeder
└── data/
    ├── schema.cat           # Master table definitions
    ├── db/                  # Binary page files (*.db) — generated by seed
    └── idx/                 # Index files (*.idx) — generated by seed
```

---

##  Build Targets

```bash
make              # Build the binary
make run          # Build + ensure runtime dirs + run
make clean        # Remove all build artifacts
make list         # Print all source files that will be compiled
```

For the interactive developer menu (build / fresh-start / reseed / smoke-test):

```bash
bash cinebook_dev.sh
```

---

## 🎟️ Promo Codes (Seeded)

| Code | Type | Value | Eligible Roles | Min Seats | Expiry |
|------|------|-------|----------------|-----------|--------|
| `WELCOME10` | % | 10% off (cap ₹100) | All | 1 | 2026-12-31 |
| `STUDENT50` | Flat | ₹50 off | Students only | 1 | 2026-12-31 |
| `GROUP15` | % | 15% off (cap ₹300) | All | 4 | 2026-12-31 |
| `WEEKEND75` | Flat | ₹75 off | User, Admin | 1 | 2026-06-30 |
| `ADMIN25` | % | 25% off | Admin only | 1 | 2026-12-31 |
| `HOLI100` | Flat | ₹100 off | All | 2 | 2026-03-15 |
| `NEWUSER20` | % | 20% off (cap ₹200) | All | 1 | 2026-12-31 |

---

## 🔧 Troubleshooting

**"cinebook: command not found" after `make run`**  
Make sure you're in the project root directory where `Makefile` lives.

**Blank screen or garbled characters on Windows**  
Run inside **Windows Terminal** or the **MSYS2 MinGW 64-bit** terminal — these support ANSI escape codes. Avoid the legacy `cmd.exe` window.

**`libcurl` not found during build**  
On Ubuntu/Debian: `sudo apt install libcurl4-openssl-dev`  
On MSYS2: `pacman -S mingw-w64-x86_64-curl-openssl`  
On macOS: `brew install curl`

**"No movies found" after launch**  
You need to seed the database first: `gcc -std=c11 -Wall -O2 -o seed tools/seed.c && ./seed`  
Or use `bash run.sh` which does this automatically.

**TMDB import fails / "SSL pre-check failed"**  
This is usually a certificate issue on Windows. Run `pacman -S mingw-w64-x86_64-curl-openssl` in MSYS2 to ensure you have the OpenSSL-backed curl build. A working internet connection is required for TMDB features.

**Crash on startup / WAL recovery message**  
This is normal after an unclean shutdown. The WAL engine will roll back any uncommitted transactions and resume safely.

---

##  Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feat/your-feature`
3. Keep all `.c` files in **C11** — no C++ in `.c` files
4. C++ additions go **only** in `src/reports/reports.cpp`
5. Commit with a conventional prefix: `feat:`, `fix:`, `docs:`, `refactor:`
6. Open a Pull Request

Please make sure `make` succeeds with zero warnings before submitting.

---

## 📄 License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for details.

---

##  Acknowledgements

- [TMDB](https://www.themoviedb.org/) — free movie metadata API
- [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble — MIT-licensed JSON parser
- [libcurl](https://curl.se/libcurl/) — HTTP client library

---

<div align="center">

 **A Structured & Object-Oriented Programming** project

</div>
