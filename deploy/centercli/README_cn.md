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

### show

| 命令 | 说明 |
|------|------|
| `show ip_white` | 接入 IP 白名单 |
| `show subscription` | 各节点类型订阅关系 |
| `show subscription <node_type>` | 指定类型的订阅 |
| `show nodes` | 在线节点 |
| `show nodes <node_type>` | 指定类型的在线节点 |
| `show center` | Center 节点主备 / 在线 |

## 示例

```bash
python3 centercli/centercli.py --url http://127.0.0.1:26000/admin show nodes
python3 centercli/centercli.py --url http://127.0.0.1:26000/admin show center
python3 centercli/centercli.py --url http://127.0.0.1:26000/admin show subscription
```

交互模式（输入 `quit` / `exit` 退出）：

```bash
python3 centercli/centercli.py --url http://127.0.0.1:26000/admin
```