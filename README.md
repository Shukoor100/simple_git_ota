# simple_git_ota

Over-the-air firmware updates for an ESP32, using nothing but GitHub.

Every merge to `main` builds the firmware in GitHub Actions and publishes it to GitHub Pages. Devices poll that site, and when it advertises a newer version they download it, install it and reboot. Failed updates roll back automatically.

- **Target:** ESP32 (4 MB flash), ESP-IDF 5.4
- **Hosting:** GitHub Pages (free, HTTPS)
- **Wi-Fi setup:** the device opens its own access point with a small setup page. No phone app.

---

## How it works

```
 push / merge to main
        │
        ▼
 GitHub Actions (build.yml)
   ├─ build firmware with ESP-IDF 5.4
   ├─ version = workflow run number
   └─ publish to GitHub Pages:  firmware.bin  version.txt  index.html
        │
        ▼
 ESP32 polls  <site>/version.txt  every few minutes
   └─ if server version > running version → download firmware.bin → reboot
```

### Versioning

The firmware version is an integer: the **run number of the `build.yml` workflow**. CI writes it to `fw_version.txt` before building, and CMake uses it as `PROJECT_VER`.

- Local builds have no `fw_version.txt`, so they are **version 0**. A device running a local build will update to the first CI build it sees.
- Re-running a workflow keeps its old number. Every new run, including a manual *Run workflow*, takes the next number.
- Pull-request checks use a separate workflow (`pr-check.yml`) so they do not consume version numbers.

### Update safety

- Downloads use HTTPS and the server certificate is checked against the Mozilla CA bundle.
- Two OTA slots: the new image is written to the inactive slot, so the running firmware is never overwritten.
- Rollback is enabled. A freshly installed image is only *confirmed* after it boots, joins Wi-Fi and reaches the update server. If it crashes or never gets there, the bootloader goes back to the previous image.
- A version that was rolled back is remembered and **not retried**. Publish a newer version to move past it.

> The firmware is **not signed**. Anyone who can push to `main` (or control the Pages site) controls what your devices run. Protect the branch, and add Secure Boot / signed updates if the devices matter.

---

## Repository layout

| Path | Purpose |
| --- | --- |
| `main/main.c` | `app_main`: starts NVS, networking, OTA and Wi-Fi, then runs your application loop (blinks the on-board LED) |
| `main/wifi.c`, `wifi.h` | Wi-Fi station, setup access point and web server |
| `main/prov.html` | The Wi-Fi setup page, embedded into the firmware |
| `main/ota.c`, `ota.h` | Version check, download, install, rollback handling |
| `main/Kconfig.projbuild` | Project settings (`OTA Configuration` menu) |
| `partitions.csv` | Flash layout with two OTA slots |
| `sdkconfig.defaults` | Default build configuration |
| `web/index.html` | Template for the status page published on GitHub Pages |
| `.github/workflows/build.yml` | Build and deploy on every push to `main` |
| `.github/workflows/pr-check.yml` | Build-only check for pull requests |

---

## Getting started

### 1. Set up GitHub

1. Create a **public** repository (GitHub Pages is not available for private repositories on free plans) and push this project to it.
2. In the repo go to **Settings → Pages → Build and deployment → Source** and choose **GitHub Actions**.
3. Push to `main` (or run the workflow manually). After the **Build ESP-IDF firmware and publish OTA site** workflow finishes, these exist:
   - `https://<user>.github.io/<repo>/version.txt`
   - `https://<user>.github.io/<repo>/firmware.bin`
   - `https://<user>.github.io/<repo>/` (status page with version, commit, size and SHA-256)

CI sets `CONFIG_OTA_BASE_URL` to match your repo automatically. For your first USB flash, set it by hand (next step).

### 2. Build and flash the first time

Install [ESP-IDF 5.4](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/get-started/index.html). Open an **ESP-IDF PowerShell/terminal** (running `idf.py` from Git Bash does not work on Windows), then:

```powershell
# one-time: point the build at your site (or edit sdkconfig.defaults)
#   CONFIG_OTA_BASE_URL="https://<user>.github.io/<repo>"

idf.py build
idf.py -p COM5 flash monitor      # use your serial port
```

Every device must be flashed this way once. After that, updates arrive over Wi-Fi.

### 3. Connect the device to Wi-Fi

With no saved Wi-Fi, the device starts its own network:

| | |
| --- | --- |
| Network name | `MyOTA` |
| Password | `12345678` |
| Setup page | `http://192.168.4.1` |

1. Join `MyOTA` on a phone or laptop.
2. Open `http://192.168.4.1` in a browser (there is no automatic captive-portal pop-up).
3. Enter your Wi-Fi name and password and press **Save**.
4. The device stores them in flash and connects. `MyOTA` switches off a few seconds after it gets an IP.

If the credentials are wrong, the device gives up after 5 attempts, forgets them and `MyOTA` comes back so you can try again.

**Changing networks:** hold **GPIO0 (the BOOT button)** low for 3 seconds while powering up. This clears the saved Wi-Fi and reopens `MyOTA`.

> `MyOTA` uses a fixed password that is visible in this repository. It is only up while the device has no Wi-Fi, but anyone nearby who reads the source could join it during that time.

---

## Releasing an update

1. Create a branch, make your change and open a pull request into `main`.
2. The **PR build check** must pass.
3. Merge. `build.yml` builds the new version and deploys it.
4. Devices pick it up on their next check (see *Configuration*), install it, reboot, and confirm it.

Watch a device with `idf.py monitor`. You should see lines like:

```
ota: running v1, server has v2
ota: Updating to v2 from https://<user>.github.io/<repo>/firmware.bin
ota: Update written, rebooting
ota: New firmware confirmed
```

### Require the build before merging

The PR workflow alone does not block a merge. Add a ruleset: **Settings → Rules → Rulesets → New branch ruleset**, target `main`, enable **Require status checks to pass** and add `build-firmware`. The check only appears in the list after it has run once on a PR.

---

## Writing your application

Put your code in `app_main` in [main/main.c](main/main.c), under the marker comment. The example loop blinks the on-board LED on GPIO2 and logs the running version every 10 seconds.

**Keep these three calls in every build you ever ship:**

```c
ota_init();
wifi_init();
ota_start_task();
```

A build without them can no longer connect to Wi-Fi or update itself, and the only fix is a USB flash.

---

## Configuration

Run `idf.py menuconfig` and open **OTA Configuration**.

| Option | Default | Meaning |
| --- | --- | --- |
| `OTA_BASE_URL` | `https://Shukoor100.github.io/simple_git_ota` | Site folder that contains `version.txt` and `firmware.bin` (no trailing slash) |
| `OTA_CHECK_INTERVAL_MIN` | `1` | How often to check for an update, in minutes (1 to 1440). A few seconds of random delay is added so devices do not all connect at once. Raise this for real deployments. |
| `PROV_RESET_GPIO` | `0` | Pin that clears saved Wi-Fi when held low for 3 s at power-up |

The on-board LED pin is `BUILD_IN_LED` in `main.c` (GPIO2 on most ESP32 dev boards).

**Flash layout** (`partitions.csv`, 4 MB):

| Name | Offset | Size |
| --- | --- | --- |
| nvs | 0x9000 | 16 KB |
| otadata | 0xd000 | 8 KB |
| phy_init | 0xf000 | 4 KB |
| ota_0 | 0x10000 | 1.94 MB |
| ota_1 | 0x200000 | 1.94 MB |

---

## Troubleshooting

| Symptom | Likely cause and fix |
| --- | --- |
| Build fails with "Partitions overlap" | A partition in `partitions.csv` runs into the next one. Each offset must be at or after the previous partition's end. |
| `idf.py` prints "MSys/Mingw is no longer supported" and does nothing | Use an ESP-IDF PowerShell or Command Prompt, not Git Bash. |
| `CONFIG_..._undeclared` after editing Kconfig | Run `idf.py reconfigure` once. Switching ESP-IDF versions needs `idf.py fullclean`. |
| Workflow fails at the deploy step | Pages source is not set to **GitHub Actions**, or the repo is private on a free plan. |
| No Pages section in Settings | Repo is private on a free plan. Make it public. |
| Device never updates | Check `CONFIG_OTA_BASE_URL`, open `<site>/version.txt` in a browser, and watch the serial log. The device only updates when the server version is **higher** than its own and not a blocked (rolled-back) version. |
| Device updated, then went back to the old version | The new image did not reach the server after booting and was rolled back. The log says `vN failed and was rolled back`. Fix the problem and publish a higher version. |
| `MyOTA` network does not appear | The device already has saved Wi-Fi. Hold BOOT (GPIO0) for 3 s at power-up to clear it. |

---

## License

No license has been chosen yet. Add a `LICENSE` file before sharing the code.
