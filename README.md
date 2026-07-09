# AE UTF-8 Fixer

[](https://github.com/AdoreLowen/AE-UTF8-Fixer/actions)
[](https://opensource.org/licenses/MIT)

**AE UTF-8 Fixer** 是一个用于解决 Windows 端 Adobe After Effects 26.2 及以上版本中部分第三方插件在中文系统环境下出现参数名、下拉菜单等乱码或不显示的辅助插件。

---

## 一、使用方法

1. 前往 [Releases](https://github.com/AdoreLowen/AE-UTF-8-Fixer/releases) 页面下载编译好的 `!00_UTF8_Fixer.aex`。
2. 将该文件放入 Adobe After Effects 的插件目录中，通常路径为：
  `C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins`
3. 重新启动 After Effects 即可生效。

---

## 二、技术原理

本插件通过以下机制实现乱码修复：

- 利用 [MinHook](https://github.com/TsudaKageyu/minhook) 库拦截 AE 宿主的参数添加函数 `add_param`。
- 在参数传入 AE UI 绘制前，自动检测字符串编码。
- 如果检测到非 UTF-8 的本地编码（如 GBK），则动态将其转换为标准的 UTF-8 编码，从而避免乱码。

---

## 三、编译方法

如果您想自行编译本项目：

1. 克隆本仓库：
  
  ```bash
  git clone --recursive https://github.com/AdoreLowen/AE-UTF-8-Fixer.git
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
