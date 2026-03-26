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

**A feature-complete, terminal-based cinema booking platform built from scratch in C/C++**

[![Language](https://img.shields.io/badge/Language-C11%20%2F%20C%2B%2B17-blue?style=flat-square&logo=c)](#)
[![Engine](https://img.shields.io/badge/Database-Custom%20RDBMS-green?style=flat-square)](#)
[![API](https://img.shields.io/badge/API-TMDB%20Live%20Data-yellow?style=flat-square)](#)
[![Build](https://img.shields.io/badge/Build-gcc%20%2F%20g%2B%2B-orange?style=flat-square&logo=gnu)](#)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows%20(MSYS2)-lightgrey?style=flat-square)](#)
[![License](https://img.shields.io/badge/License-MIT-purple?style=flat-square)](#)

*Your Cinema. Your Seats. Your Story.*

</div>

---

## 🌟 What is CineBook?

CineBook is a **fully functional, terminal-based cinema booking system**. It simulates a real-world booking platform with live movie data pulled from [The Movie Database (TMDB)](https://www.themoviedb.org/), backed by a **custom RDBMS engine** with page storage, write-ahead logging (WAL), and in-memory hash/sorted indexes.

No SQLite. No Python. No third-party database. Every byte on disk is managed by code written from scratch.

---

## ✨ Feature Highlights

| Category | Features |
|----------|----------|
| 🎥 **Live Movie Data** | TMDB API integration — search & import real movies with cast, synopsis, genre, runtime |
| 🏟️ **Theatre Management** | Multi-city, multi-theatre, multi-screen support (2D / IMAX 2D / IMAX 3D / 4DX) |
| 💺 **Interactive Seat Map** | ANSI colour-coded 2D grid — select/deselect seats, live availability, lazy hold expiry |
| 💰 **Dynamic Pricing** | 12-slot breakdown: screen surcharge, seat type multiplier, weekend/peak surge, GST, group & student discounts |
| 🎟️ **Promo Codes** | Percentage and flat-rate codes with role masks, usage limits, and date ranges |
| 💳 **Payment Simulation** | Wallet, UPI (VPA validation), Card (Luhn check), Net Banking — all simulated realistically |
| 🔄 **Refund Engine** | Time-tiered refund policy (100% / 75% / 50% / 0%) with atomic WAL-backed cancellation |
| 📊 **Admin Analytics** | Occupancy, Revenue, and Booking Trend reports via C++ OOP layer with ANSI bar charts |
| 📥 **CSV Export** | One-click dashboard export to `exports/` directory |
| 🔐 **Auth System** | SHA-256 password hashing (pure C, no OpenSSL), academic domain detection for student upgrades |
| 🔔 **Waitlist & Notifications** | Auto-notify waitlisted users when seats free up |
| 💾 **Custom RDBMS** | Page-based binary storage, buffer pool (LRU), WAL crash recovery, hash + sorted indexes |

---

## 🏗️ Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                         main.c  (Entry)                          │
├────────────────────────────┬─────────────────────────────────────┤
│   Auth Layer               │   UI Layer                          │
│   auth.c / session.c       │   ui_browse, ui_booking,            │
│   SHA-256 · Roles          │   ui_cancel, ui_account,            │
│   SessionContext*          │   ui_admin, ui_dashboard            │
├────────────────────────────┴─────────────────────────────────────┤
│                    Business Logic Layer (C)                       │
│   pricing.c · refund.c · payment.c · promos.c · location.c       │
├───────────────────────────────────────────────────────────────────┤
│              C++ OOP Layer  (reports.cpp)                         │
│   Report ABC → OccupancyReport / RevenueReport / BookingReport   │
│   TMDBClient  (libcurl + cJSON)  · TMDBException                 │
├───────────────────────────────────────────────────────────────────┤
│                   RDBMS Engine Layer (C)                          │
│   query.c  ←→  schema.c  ·  storage.c  ·  index.c  ·  txn.c    │
│   record.c (serialize / deserialize)                              │
├───────────────────────────────────────────────────────────────────┤
│                        Data Layer                                 │
│   *.db (binary pages)  ·  *.idx  ·  schema.cat  ·  wal.log       │
└───────────────────────────────────────────────────────────────────┘
```

The system runs as a **single process** — no threads, no external database. All "real-time" feel is achieved through careful state management and lazy evaluation.

---

## 📂 Project Structure

```
CineBook/
├── main.c                  # Entry point, bootstrap, auth loop
├── Makefile                # Cross-platform (Linux / macOS / MSYS2)
├── Cinebook.conf           # Runtime config (TMDB key, default city, currency)
├── cinebook_dev.sh         # Developer helper (build / seed / run / test)
├── lib/
│   ├── cJSON.c / .h        # Lightweight JSON parser
├── src/
│   ├── engine/             # Custom RDBMS (storage, record, schema, index, txn, query)
│   ├── auth/               # Authentication & session management
│   ├── logic/              # Business rules (pricing, refund, payment, promos, location)
│   ├── reports/            # C++ OOP layer (reports + TMDB client)
│   └── ui/                 # All terminal UI modules
├── tools/
│   └── seed.c              # Database seeder (17 theatres, 38 screens, 7 cities)
└── data/
    ├── schema.cat          # Master table definitions
    ├── db/                 # Binary page files (.db)
    └── idx/                # Index files (.idx)
```

---

## ⚡ Quick Start

### Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| `gcc` | ≥ 9.0 | For all `.c` files (C11) |
| `g++` | ≥ 9.0 | For `reports.cpp` (C++17) + linker |
| `libcurl` | ≥ 7.x | For TMDB API calls |
| `make` | Any | Build system |

---

### 🐧 Linux (Ubuntu / Debian)

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev git

# 2. Clone the repo
git clone https://github.com/YOUR_USERNAME/cinebook.git
cd cinebook

# 3. Build
make

# 4. Seed the database (run once)
gcc -std=c11 -Wall -O2 -o seed tools/seed.c
./seed

# 5. Run
make run
```

---

### 🍎 macOS

```bash
# 1. Install dependencies (Homebrew)
brew install gcc curl

# 2. Clone
git clone https://github.com/YOUR_USERNAME/cinebook.git
cd cinebook

# 3. Build + Seed + Run
make
gcc -std=c11 -Wall -O2 -o seed tools/seed.c && ./seed
make run
```

---

### 🪟 Windows (MSYS2 / MinGW64)

```bash
# 1. Install MSYS2 from https://www.msys2.org/
# 2. Open "MSYS2 MinGW 64-bit" terminal

pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-curl-openssl \
          make git

# 3. Clone & build
git clone https://github.com/YOUR_USERNAME/cinebook.git
cd cinebook
make

# 4. Seed
gcc -std=c11 -Wall -O2 -o seed tools/seed.c && ./seed

# 5. Run
make run
```

> **Tip:** Use the developer helper script for a guided experience:
> ```bash
> bash cinebook_dev.sh
> ```
> Choose **F** (Fresh Start) on first run — it builds, seeds, and launches automatically.

---

### 🔑 TMDB API Key (Optional but Recommended)

CineBook works out of the box with the seeded dataset. To enable **live movie imports**:

1. Create a free account at [themoviedb.org](https://www.themoviedb.org/settings/api)
2. Generate an API key (v3 auth)
3. Open `Cinebook.conf` and replace the key:

```ini
TMDB_API_KEY=your_api_key_here
```

4. In the Admin Panel → Movie Management → **SUPER IMPORT** to pull all currently-showing titles (India), or search by name.

---

## 🚀 Usage Guide

### Default Credentials (Seeded)

| Role | Phone | Password |
|------|-------|----------|
| 🔑 **Admin** | `9000000001` | `admin123` |
| 👤 **User** | `9876543210` | `pass1234` |
| 🎓 **Student** | `9845123456` | `pass1234` |

### User Flow

```
Login / Sign Up
    └─▶  Browse Movies (genre / language filter)
              └─▶  View Details (cast, synopsis, streaming platforms)
                        └─▶  Select Show (city-filtered)
                                  └─▶  Interactive Seat Map
                                            └─▶  Price Breakdown (12-slot)
                                                      └─▶  Apply Promo Code
                                                                └─▶  Pay (Wallet / UPI / Card / Net Banking)
                                                                          └─▶  Booking Confirmed ✓
```

### Admin Flow

```
Admin Login
    ├─▶  Movie Management   — TMDB import / manual add / Super Import
    ├─▶  Theatre Management — Add/edit theatres, screens, seats
    ├─▶  Show Management    — Schedule shows, set prices, cancel with bulk refund
    ├─▶  Promo Management   — Create/deactivate promo codes
    ├─▶  Analytics          — Occupancy / Revenue / Booking Trends reports
    └─▶  System             — Manage cities, academic domains, refund policy tiers
```

---

## 🛠️ Build Targets

```bash
make          # Build the binary
make run      # Build + create runtime dirs + run
make clean    # Remove build artifacts
make list     # Show all source files that will be compiled
```

---

## 📦 Seeded Dataset

The seeder (`tools/seed.c`) generates a production-like dataset:

| Entity | Count |
|--------|-------|
| Cities | 10 (Chennai, Mumbai, Delhi, Bengaluru, Hyderabad, Kolkata, Pune…) |
| Theatres | 17 |
| Screens | 38 (2D / IMAX 2D / IMAX 3D / 4DX) |
| Seats | ~6,010 |
| Shows | 114 (3 per screen, configurable date) |
| Movies | 8 (Kalki 2898-AD, Stree 2, Pushpa 2, Devara…) |
| Promo Codes | 7 (WELCOME10, STUDENT50, GROUP15, HOLI100…) |
| Academic Domains | 20 (IIT, BITS, VIT, NIT…) |

---

## 🔬 Technical Deep-Dive

### Custom RDBMS
- **Page size:** 4096 bytes — 8-byte header + 4088 bytes data
- **Buffer pool:** 64-page LRU cache — dirty pages flushed on eviction
- **WAL:** Full page before/after images, XOR checksum, crash recovery on startup
- **Indexes:** Open-address hash table (O(1) PK lookup) + sorted array (O(log n) range)
- **Serialization:** Fixed-offset binary layout per `schema.cat`; NULL sentinels per type

### C++ OOP Layer
- `Report` abstract base class → `OccupancyReport`, `RevenueReport`, `BookingReport`
- `TMDBClient` encapsulates all HTTP calls (libcurl); `TMDBException : std::runtime_error`
- STL: `std::vector`, `std::map`, `std::sort`, `std::unique_ptr`
- C-callable wrappers with `extern "C"` guards keep the boundary clean

### Pricing Engine
Dynamic 12-slot breakdown computed fresh at every hold:
```
base_price → screen_surcharge → seat_type_mult → subtotal_per_seat
→ student_discount → group_discount → promo_discount → dynamic_surge
→ taxable_amount → GST(18%) → convenience_fee → grand_total
```

---

## 🤝 Contributing

1. Fork the repo
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit your changes: `git commit -m "feat: add my feature"`
4. Push to the branch: `git push origin feature/my-feature`
5. Open a Pull Request

Please keep `.c` files in C11 and only add C++ to `reports.cpp`.

---

## 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgements

- [The Movie Database (TMDB)](https://www.themoviedb.org/) for the free movie API
- [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble — lightweight JSON parser
- [libcurl](https://curl.se/libcurl/) — HTTP client library

---

<div align="center">

Made with ♥ for BACSE104 — Structured & Object-Oriented Programming

</div>
