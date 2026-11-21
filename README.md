# AE UTF-8 Fixer

[![Build and Release AE Plugin](https://github.com/AdoreLowen/AE-UTF8-Fixer/actions/workflows/build.yml/badge.svg)](https://github.com/AdoreLowen/AE-UTF8-Fixer/actions)
[![Release](https://img.shields.io/github/v/release/AdoreLowen/AE-UTF8-Fixer)](https://github.com/AdoreLowen/AE-UTF8-Fixer/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/AdoreLowen/AE-UTF8-Fixer/total)](https://github.com/AdoreLowen/AE-UTF8-Fixer/releases)
[![Stars](https://img.shields.io/github/stars/AdoreLowen/AE-UTF8-Fixer?style=social)](https://github.com/AdoreLowen/AE-UTF8-Fixer/stargazers)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows&logoColor=white)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**AE UTF-8 Fixer** 是一个用于解决 Windows 端 Adobe After Effects 中部分第三方插件在中文 / 日文 / 韩文等 CJK 系统环境下出现参数名、下拉菜单等乱码或不显示的辅助插件（主要针对 AE 26.2 及以上版本）。

---

## 一、使用方法

1. 前往 [Releases](https://github.com/AdoreLowen/AE-UTF8-Fixer/releases) 页面下载编译好的 `!00_UTF8_Fixer.aex`。
2. 将该文件放入 Adobe After Effects 的插件目录中（不要重命名文件），路径形如：
  `C:\Program Files\Adobe\Adobe After Effects <版本>\Support Files\Plug-ins`
  例如 `...\Adobe After Effects 2026\Support Files\Plug-ins`。
3. 重新启动 After Effects 即可生效。

---

## 二、技术原理

本插件通过以下机制实现乱码修复：

- 启动时自动检测 AE 版本与系统 CJK 代码页（936/950/932/949），决定修复方向。
- 利用 [MinHook](https://github.com/TsudaKageyu/minhook) 库 hook AE 宿主的 `add_param`、`PF_UpdateParamUI`，并 hook 各插件入口点与 `LoadLibrary`，以覆盖后续加载的插件。
- 在字符串传入 AE UI 绘制前自动检测编码，并按版本在「本地编码 ↔ UTF-8」两个方向间转换：
  - AE 26.2 ~ 26.x：本地编码（如 GBK）→ UTF-8；
  - AE 27.0+ 及更早版本：UTF-8 → 本地编码。
- 对已声明 `PF_OutFlag2_STRINGS_ARE_UTF8` 的插件自动跳过，避免误转换。

---

## 三、编译方法

如果您想自行编译本项目：

1. 克隆本仓库：
  
  ```bash
  git clone --recursive https://github.com/AdoreLowen/AE-UTF8-Fixer.git
  ```
  
2. 确保在本地存放有 `AE_SDK`，并配置好 `CMakeLists.txt` 中的 SDK 路径。
  
3. 使用 CMake 进行构建：
  
  ```bash
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
  cmake --build build --config Release
  ```
  

---

## 四、开源协议

本项目基于 [MIT License](LICENSE) 协议开源。
