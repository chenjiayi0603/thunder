#!/usr/bin/env python3
"""Center 管理 CLI：仅保留常用 show / get / set（框架节点配置）。"""
import os
import json
import base64

import click
import requests


def _client(ctx: click.Context) -> "Centercli":
    """子命令的 ctx.obj 常为 None，实例在父级 group 的 ctx.obj 上。"""
    if ctx.obj is not None:
        return ctx.obj
    if ctx.parent is not None and ctx.parent.obj is not None:
        return ctx.parent.obj
    raise click.ClickException("内部错误：未初始化客户端（请使用 --url）")


class Centercli:
    SHOW = frozenset({"ip_white", "subscription", "nodes", "center"})
    GET = frozenset({"node_config", "node_custom_config"})
    SET = frozenset({"node_config_from_file", "node_custom_config_from_file"})

    def __init__(self, url: str):
        self.url = url

    def exec_cmd(self, req_json: str) -> str:
        # Session.trust_env=False：避免环境变量代理指向 socks 且未装 PySocks 时报错；访问本机 Center 时更稳
        with requests.Session() as s:
            s.trust_env = False
            return s.post(self.url, data=req_json, timeout=120).text

    def show(self, param: list) -> None:
        if not param or param[0] not in self.SHOW:
            click.secho(f"invalid show target: {param[0] if param else '(empty)'}", fg="red")
            return

        n = len(param)
        sub = param[0]

        if sub in ("ip_white", "center") and n != 1:
            click.secho(f"usage: show {sub}", fg="red")
            return
        if sub in ("subscription", "nodes") and n not in (1, 2):
            click.secho(f"usage: show {sub} [node_type]", fg="red")
            return

        args = list(param)
        req_json = json.dumps({"cmd": "show", "args": args}, ensure_ascii=False)
        result_string = self.exec_cmd(req_json)
        try:
            result = json.loads(result_string)
        except json.JSONDecodeError:
            click.echo(result_string)
            return

        if sub == "ip_white":
            for ip in result.get("data") or []:
                click.echo(str(ip))
        elif sub == "subscription":
            if n == 1:
                for node_type in result.get("data") or []:
                    click.echo("%s:" % node_type["node_type"])
                    for sub_node_type in node_type.get("subcriber") or []:
                        click.echo("\t%s" % sub_node_type)
            else:
                for node_type in result.get("data") or []:
                    click.echo("%s" % node_type)
        elif sub == "nodes":
            if n == 1:
                for block in result.get("data") or []:
                    click.echo("%s:" % block["node_type"])
                    for node in block.get("node") or []:
                        click.echo("\t%s" % node)
            else:
                for node in result.get("data") or []:
                    click.echo("%s" % node)
        elif sub == "center":
            data = result.get("data") or []
            if data:
                click.echo("node\tis_leader\tis_online")
            for node in data:
                click.echo(
                    "%s\t%s\t%s"
                    % (node["identify"], node["leader"], node["online"])
                )

    def get(self, param: list) -> None:
        if not param or param[0] not in self.GET:
            click.secho(f"invalid get target: {param[0] if param else '(empty)'}", fg="red")
            return
        if len(param) != 2:
            click.secho("usage: get node_config <node_identify>", fg="red")
            click.secho("       get node_custom_config <node_identify>", fg="red")
            return

        req_json = json.dumps(
            {"cmd": "get", "args": [param[0], param[1]]}, ensure_ascii=False
        )
        result_string = self.exec_cmd(req_json)
        try:
            result = json.loads(result_string)
        except json.JSONDecodeError:
            click.secho(result_string, fg="red")
            return
        if result.get("code") == 0:
            raw = result["data"]["file_content"]
            file_content = base64.b64decode(raw.encode("utf-8")).decode("utf-8")
            click.echo(file_content)
        else:
            click.secho(result_string, fg="red")

    def set(self, param: list) -> None:
        if not param or param[0] not in self.SET:
            click.secho(f"invalid set target: {param[0] if param else '(empty)'}", fg="red")
            return

        sub = param[0]
        n = len(param)
        path = param[-1]

        if not os.path.isfile(path) or os.stat(path).st_size == 0:
            click.secho("file missing or empty: %s" % path, fg="red")
            return

        with open(path, "rb") as f:
            read_content = f.read()
        file_b64 = base64.b64encode(read_content).decode("utf-8")

        if sub == "node_config_from_file":
            if n == 3:
                args = [sub, param[1], file_b64]
            elif n == 4:
                args = [sub, param[1], param[2], file_b64]
            else:
                click.secho(
                    "usage: set node_config_from_file <node_type> <file>\n"
                    "       set node_config_from_file <node_type> <node_identify> <file>",
                    fg="red",
                )
                return
        else:  # node_custom_config_from_file
            if n == 3:
                args = [sub, param[1], file_b64]
            elif n == 4:
                args = [sub, param[1], param[2], file_b64]
            else:
                click.secho(
                    "usage: set node_custom_config_from_file <node_type> <file>\n"
                    "       set node_custom_config_from_file <node_type> <node_identify> <file>",
                    fg="red",
                )
                return

        req_json = json.dumps({"cmd": "set", "args": args}, ensure_ascii=False)
        result_string = self.exec_cmd(req_json)
        try:
            result = json.loads(result_string)
        except json.JSONDecodeError:
            click.echo(result_string)
            return
        if result.get("code") == 0:
            click.echo(result_string)
        else:
            click.secho(result_string, fg="red")


@click.group(invoke_without_command=True)
@click.option(
    "--url",
    "-r",
    prompt="url",
    help="Center ModuleAdmin HTTP 地址，例如 http://host:port/admin",
)
@click.pass_context
def cli(ctx, url):
    ctx.obj = Centercli(url)
    if ctx.invoked_subcommand is None:
        while True:
            line = click.prompt("centercli", prompt_suffix="> ")
            parts = line.strip().split()
            if not parts:
                continue
            cmd = parts[0]
            if cmd in ("quit", "exit"):
                raise SystemExit(0)
            if cmd == "show":
                ctx.obj.show(parts[1:]) if len(parts) > 1 else click.secho(
                    'usage: show <ip_white|subscription|nodes|center> ...', fg="red"
                )
            elif cmd == "get":
                ctx.obj.get(parts[1:]) if len(parts) > 1 else click.secho(
                    "usage: get node_config <identify> | get node_custom_config <identify>",
                    fg="red",
                )
            elif cmd == "set":
                ctx.obj.set(parts[1:]) if len(parts) > 1 else click.secho(
                    "usage: set node_config_from_file ... | set node_custom_config_from_file ...",
                    fg="red",
                )
            else:
                click.secho('unknown command (use: show | get | set | quit)', fg="red")


@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def show(ctx, args):
    """查看白名单、订阅、在线节点、Center 主备。"""
    _client(ctx).show(list(args))


@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def get(ctx, args):
    """拉取节点框架配置 / 自定义配置（base64 解码后打印）。"""
    _client(ctx).get(list(args))


@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def set(ctx, args):
    """从本地文件推送 node_config / node_custom_config。"""
    _client(ctx).set(list(args))


if __name__ == "__main__":
    cli()
