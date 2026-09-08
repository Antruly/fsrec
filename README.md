# fsrec — NTFS 数据恢复工具

一款从零实现的 NTFS 数据恢复工具：通过 Windows 原始磁盘读取接口解析 NTFS 文件系统，
重建目录树，支持扫描（解析 `$MFT`）、浏览/搜索、按原目录结构**选择性恢复**已删除或误格式化后仍存留的文件。

- **后端**：C++17 + [libuvcpp](https://github.com/zhuweiye/libuvcpp)（HTTP/WebSocket 框架，仅引用不修改）+ [nlohmann/json](https://github.com/nlohmann/json) + Windows API
- **前端**：单文件原生 HTML/CSS/JS（`frontend/dist/index.html`，由 `uvcpp_static_server` 直接托管，无构建步骤）
- **版本**：v1.0.1（64 位 Windows）

> ⚠️ **安全须知**：本工具需**管理员权限**，会直接读取原始磁盘扇区。源盘全程只读；
> 恢复输出目录必须位于**与源盘不同的磁盘**。请勿在存有重要数据的生产机上随意测试，
> 详见 `docs/免责声明.md`。

---

## 功能

| 阶段 | 说明 |
|------|------|
| 磁盘枚举 | 列出物理磁盘与盘符，识别系统盘（磁盘 0，禁止选择） |
| 引导扇区解析 | 读取 BPB（字节/扇区、扇区/簇、`$MFT` LCN、记录大小等） |
| `$MFT` 解析 | 遍历文件记录，USA 修复，提取 `$STANDARD_INFORMATION`、`$FILE_NAME`、`$DATA` |
| 数据运行 | 解析 resident / non-resident 属性，含稀疏簇处理 |
| 目录树重建 | 依据父引用与文件名（含 `$MFT` 编号兜底）构建树 |
| 分区级扫描 | `raw_scan_all` 遍历整盘、定位每个 NTFS 分区并分别缓存 |
| 搜索 | 前端按名称/类型过滤 |
| 选择性恢复 | 保留原目录结构写入输出目录，自动改名同名文件；支持**暂停/继续/停止** |
| 实时进度 | WebSocket 推送：总体进度 + 当前文件 + 每文件进度 |
| 安全与空间 | 源盘锁定（目录选择器禁用源盘）、空间预估与不足警告 |

## 目录结构

```
fsrec/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                 # 入口：管理员检测 + 参数解析 + 启动服务 + 自动开浏览器
│   ├── include/
│   │   ├── types.h / util.h / util.cpp / version.h
│   ├── ntfs/                    # disk_reader / boot_parser / data_run / mft_parser / tree_builder
│   ├── recovery/                # scanner（后台扫描）/ restorer（后台恢复）
│   └── http/server.*            # 路由 + WebSocket 推送中心
├── resources/
│   ├── icon.png                  # 图标母版（make_icon.ps1 用它生成 fsrec.ico；.psd 源文件已排除）
│   ├── fsrec.ico                # 由 make_icon.ps1 生成的多尺寸图标
│   ├── fsrec.manifest           # requireAdministrator 提权清单
│   └── fsrec.rc                 # 版本信息 + 图标资源
├── frontend/
│   └── dist/index.html          # 线上前端（单文件；frontend/src 为废弃的 Vue 残留）
├── docs/
│   ├── 使用说明.md               # 用户手册（操作流程 + 注意事项 + FAQ）
│   └── 免责声明.md
├── packaging/
│   ├── fsrec.iss                # Inno Setup 安装脚本（全中文）
│   └── languages/ChineseSimplified.isl
├── scripts/
│   ├── make_icon.ps1            # icon.png → fsrec.ico
│   └── build_release.ps1        # 一键构建 + 打包安装程序
├── 启动fsrec.bat / 停止fsrec.bat
└── LICENSE / README.md
```

## 构建

前置条件：Windows、Visual Studio 2022（MSVC，需匹配预编译 libuvcpp 二进制）、CMake ≥ 3.20。
`libuvcpp` 位于 `D:\public\libuvcpp`（仅引用，不修改），路径在 `CMakeLists.txt` 的 `LIBUVCPP_ROOT`。

```powershell
cd D:\public\fsrec
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物 `build\Release\recovery_server.exe`，运行时依赖同目录下的 `uvcpp.dll`、`uv.dll`、
`llhttp.dll`、`libssl-3-x64.dll`、`libcrypto-3-x64.dll`（post-build 自动复制前三个，
OpenSSL 两个由 libuvcpp 的 build_web 提供）。

## 运行

后端默认监听 `0.0.0.0:8080`，**需管理员权限**（exe 已嵌入 `requireAdministrator` 清单，
双击即触发 UAC；也可手动"以管理员身份运行"）：

```powershell
D:\public\fsrec\build\Release\recovery_server.exe          # 启动并自动打开浏览器
D:\public\fsrec\build\Release\recovery_server.exe --no-browser   # 不自动开浏览器
D:\public\fsrec\build\Release\recovery_server.exe --port 8090    # 指定端口
```

健康检查：`curl http://localhost:8080/ping` → `{"status":"ok","name":"fsrec","version":"1.0.1"}`。

## 打包发布（Inno Setup 安装包）

前置条件：已安装 [Inno Setup 6](https://jrsoftware.org/isdl.php)；`scripts/build_release.ps1`
会自动下载简体中文语言文件（首次）。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

产物输出到 `release\fsrec_Setup_1.0.1.exe`（全中文安装向导，安装后开始菜单/桌面快捷方式，
双击 exe 自动提权并打开浏览器）。

## API 概览

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/ping` | 健康检查（含 name/version） |
| GET  | `/api/physical_disks` | 物理磁盘列表（disk_number / model / size） |
| POST | `/api/raw_scan_all` | `{disk_number}` 遍历整盘、定位所有分区 → 各分区独立 task |
| GET  | `/api/scans` | 已完成的扫描（含持久化缓存） |
| GET  | `/api/scan/{id}/status` | 扫描进度 |
| GET  | `/api/scan/{id}/results` | 目录树（根节点 `/`） |
| POST | `/api/scan/{id}/pause\|resume\|stop` | 暂停 / 继续 / 停止扫描 |
| POST | `/api/recover` | `{task_id, selected_paths[], output_dir}` → `{job_id}` |
| GET  | `/api/recover/{id}/status` | 恢复进度（含 current_file / current_bytes / current_size） |
| POST | `/api/recover/{id}/pause\|resume\|stop` | 暂停 / 继续 / 停止 |
| GET  | `/api/fs/drives` | 盘符 + `physical_disks[]` 映射 + 可用空间 |
| GET  | `/api/fs/list?path=` | 目录列表（选择器用） |
| POST | `/api/fs/mkdir` | `{path, name}` 新建目录 |
| GET  | `/api/fs/space?path=` | 目标卷 free/total |
| WS   | `/ws` | WebSocket 扫描 / 恢复进度实时推送 |

## 安全与注意事项

- 源盘通过 `CreateFile(GENERIC_READ)` 只读打开，不写入源盘。
- 恢复输出目录后端会校验并拒绝与源盘/系统盘相同的磁盘。
- 已删除文件的数据区可能已被覆写，不可恢复项会标注。
- 同名文件自动追加 `_1`、`_2`… 后缀，不覆盖。
- 详见 `docs/使用说明.md` 与 `docs/免责声明.md`。

## 许可证

[MIT](LICENSE) © 2026 Antruly
