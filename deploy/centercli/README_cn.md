# centercli（中文说明）

集群命令行管理工具（Python 3 + [click](https://github.com/pallets/click) + requests）。

## 安装

在 **`deploy/centercli/`** 目录下：

```bash
pip install --editable .
```

或直接使用脚本（需已安装 `click`、`requests`），**仓库根**下：

```bash
python3 deploy/centercli/centercli.py --help
```

## 指令说明（精简版）

仅保留常用 **`show` / `get` / `set`**，且 **`set` 只支持从本地文件上传**（`node_config_from_file` / `node_custom_config_from_file`）。

### show

| 命令 | 说明 |
|------|------|
| `show ip_white` | 接入 IP 白名单 |
| `show subscription` | 各节点类型订阅关系 |
| `show subscription <node_type>` | 指定类型的订阅 |
| `show nodes` | 在线节点 |
| `show nodes <node_type>` | 指定类型的在线节点 |
| `show center` | Center 节点主备 / 在线 |

### get

| 命令 | 说明 |
|------|------|
| `get node_config <node_identify>` | 拉取框架配置（打印解码后的文本） |
| `get node_custom_config <node_identify>` | 拉取自定义配置片段 |

### set

| 命令 | 说明 |
|------|------|
| `set node_config_from_file <node_type> <file>` | 按节点类型推送框架配置 |
| `set node_config_from_file <node_type> <node_identify> <file>` | 指定节点标识推送框架配置 |
| `set node_custom_config_from_file <node_type> <file>` | 按类型推送自定义配置 |
| `set node_custom_config_from_file <node_type> <node_identify> <file>` | 指定节点推送自定义配置 |

## 示例

```bash
python3 deploy/centercli/centercli.py --url http://127.0.0.1:26000/admin show nodes
python3 deploy/centercli/centercli.py --url http://127.0.0.1:26000/admin show center
python3 deploy/centercli/centercli.py --url http://127.0.0.1:26000/admin get node_config Hello.0
```

交互模式（输入 `quit` / `exit` 退出）：

```bash
python3 deploy/centercli/centercli.py --url http://127.0.0.1:26000/admin
```

在 **`deploy/`** 目录下也可：`python3 centercli/centercli.py ...`（路径按当前工作目录调整）。

## 说明

- 更复杂的子命令（如 `node_report` / `node_detail`、多参数 `custom_config` 等）已从本脚本移除；若仍需使用，可直接对 ModuleAdmin 接口 POST JSON，或与历史版本脚本对照。
- `--url` 为 Center **ModuleAdmin** 的 HTTP 地址（例如 `.../admin`），与部署中 `access_host` / `access_port` 一致。
