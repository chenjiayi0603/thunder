#!/usr/bin/env python3
# nebcli.py
import os
import json
import requests
import click
import base64


class Centercli(object):
    """
    Nebula cluster administrator command line interface.
    """
    def __init__(self, url):
        self.url = url
        self.show_sub_cmd = (
            "ip_white", "subscription", "nodes", "node_report", "node_detail", "center"
        )
        self.get_sub_cmd = (
            "node_config", "node_custom_config", "custom_config"
        )
        self.set_sub_cmd = (
            "node_config", "node_config_from_file",
            "node_custom_config", "node_custom_config_from_file",
            "custom_config", "custom_config_from_file"
        )

    def exec_cmd(self, req_json):
        response = requests.post(self.url, data=req_json)
        return response.text

    def show(self, param=[]):
        req_json = ""
        if param[0] in self.show_sub_cmd:
            param_num = len(param)
            if param_num == 1:
                if param[0] == "node_report" or param[0] == "node_detail":
                    click.secho("invalid param num for \"show %s\"!" % param[0], fg='red')
                else:
                    req_json = (
                        """
                        {
                            "cmd":"show",
                            "args":["%s"]
                        }
                        """ % param[0]
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if param[0] == "ip_white":
                        for ip in result["data"]:
                            click.echo("%s" % ip)
                    elif param[0] == "subscription":
                        for node_type in result["data"]:
                            click.echo("%s:" % node_type["node_type"])
                            for sub_node_type in node_type["subcriber"]:
                                click.echo("\t%s" % sub_node_type)
                    elif param[0] == "nodes":
                        for nodes in result["data"]:
                            click.echo("%s:" % nodes["node_type"])
                            for node in nodes["node"]:
                                click.echo("\t%s" % node)
                    elif param[0] == "center":
                        if len(result["data"]) > 0:
                            click.echo("node\tis_leader\tis_online")
                        for node in result["data"]:
                            click.echo("%s\t%s\t%s"
                                       % (node["identify"],
                                          node["leader"],
                                          node["online"]))
                    else:
                        click.echo("%s" % result_string)
            elif param_num == 2:
                if param[0] == "ip_white" or param[0] == "center":
                    click.secho("invalid param num for \"show %s\"", param[0], fg='red')
                else:
                    req_json = (
                        """
                        {
                            "cmd":"show",
                            "args":["%s", "%s"]
                        }
                        """ % tuple(param)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if param[0] == "subscription":
                        for node_type in result["data"]:
                            click.echo("%s" % node_type)
                    elif param[0] == "nodes":
                        for node in result["data"]:
                            click.echo("%s" % node)
                    elif param[0] == "node_report":
                        if len(result["data"]) > 0:
                            click.echo("nodeIp\tnode_port\tload\tconnect\t"
                                       "recvNum\trecv_byte\tsend_num\t"
                                       "sendByte\tclient")
                        """
                        {
                                "code": 0,
                                "msg":  "success.",
                                "data": [{
                                                "nodeType":     "API",
                                                "nodeId":       3,
                                                "nodeIp":       "192.168.11.66",
                                                "nodePort":     27009,
                                                "accessIp":     "192.168.11.66",
                                                "accessPort":   27008,
                                                "workerNum":    1,
                                                "activeTime":   1582806743.931121,
                                                "node": {
                                                        "load": 5,
                                                        "connect":      5,
                                                        "recvNum":      1,
                                                        "recvByte":     19,
                                                        "client":       5
                                                },
                                                "workers":      [{
                                                                "load": 5,
                                                                "connect":      5,
                                                                "recvNum":      1,
                                                                "recvByte":     19,
                                                                "client":       5
                                                        }]
                                        }]
                        }
                        """
                        
                        for node in result["data"]:
                            print(node)
                            click.echo("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d"
                                       % (node["nodeIp"],
                                          node["nodePort"],
                                          node["node"]["load"],
                                          node["node"]["connect"],
                                          node["node"]["recvNum"],
                                          node["node"]["recvByte"],
                                          node["node"]["sendNum"],
                                          node["node"]["sendByte"],
                                          node["node"]["client"]))
                    elif param[0] == "node_detail":
                        if len(result["data"]) > 0:
                            click.echo("nodeId\tnodeIp\tnodePort\t"
                                       "workerNum\tactiveTime")
                        for node in result["data"]:
                            click.echo("%d\t%s\t%d\t%d\t%f"
                                       % (node["nodeId"],
                                          node["nodeIp"],
                                          node["nodePort"],
                                          node["workerNum"],
                                          node["activeTime"]))
                    else:
                        click.echo("%s" % result_string)
            elif param_num == 3:
                if param[0] == "node_report" or param[0] == "node_detail":
                    req_json = (
                        """
                        {
                            "cmd":"show",
                            "args":["%s", "%s", "%s"]
                        }
                        """ % tuple(param)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if param[0] == "node_report":
                        if len(result["data"]) > 0:
                            click.echo("worker\tload\tconnect\t"
                                       "recvNum\trecv_byte\tsend_num\t"
                                       "sendByte\tclient")
                        for node in result["data"]:
                            click.echo("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d"
                                       % ("node",
                                          node["node"]["load"],
                                          node["node"]["connect"],
                                          node["node"]["recvNum"],
                                          node["node"]["recvByte"],
                                          node["node"]["recvByte"],
                                          node["node"]["sendByte"],
                                          node["node"]["client"]))
                            worker_no = 1
                            for worker in node["worker"]:
                                click.echo("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d"
                                           % (worker_no,
                                              worker["load"],
                                              worker["connect"],
                                              worker["recvNum"],
                                              worker["recvByte"],
                                              worker["recvByte"],
                                              worker["sendByte"],
                                              worker["client"]))
                                worker_no += 1
                    elif param[0] == "node_detail":
                        if len(result["data"]) > 0:
                            click.echo("nodeId\tnode_ip\tnode_port\t"
                                       "workerNum\tactive_time")
                        for node in result["data"]:
                            click.echo("%d\t%s\t%d\t%d\t%f"
                                       % (node["nodeId"],
                                          node["nodeIp"],
                                          node["nodePort"],
                                          node["workerNum"],
                                          node["activeTime"]))
                    else:
                        click.echo("%s" % result_string)
                else:
                    click.secho("invalid param num for \"show %s\"", param[0], fg='red')
            else:
                click.secho("invalid param num for \"show\"", fg='red')
        else:
            click.secho("invalid param: %s" % param[0], fg='red')

    def get(self, param=[]):
        req_json = ""
        if param[0] in self.get_sub_cmd:
            param_num = len(param)
            if param_num == 2:
                if param[0] == "node_config" or param[0] == "node_custom_config":
                    req_json = (
                        """
                        {
                            "cmd":"get",
                            "args":["%s", "%s"]
                        }
                        """ % tuple(param)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        file_content = base64.b64decode(result["data"]["file_content"].encode("utf-8")).decode("utf-8")
                        click.secho("%s" % file_content)
                    else:
                        click.secho("%s" % result_string, fg='red')
                else:
                    click.secho("invalid param num for \"get %s\"!" % param[0], fg='red')
            elif param_num == 4:
                if param[0] == "custom_config":
                    req_json = (
                        """
                        {
                            "cmd":"get",
                            "args":["%s", "%s", "%s", "%s"]
                        }
                        """ % tuple(param)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        file_content = base64.b64decode(result["data"]["file_content"].encode("utf-8")).decode("utf-8")
                        click.secho("%s" % file_content)
                    else:
                        click.secho("%s" % result_string, fg='red')
                else:
                    click.secho("invalid param num for \"get %s\"!" % param[0], fg='red')
            else:
                click.secho("invalid param num for \"get %s\"!" % param[0], fg='red')
        else:
            click.secho("invalid param: %s" % param[0], fg='red')

    def set(self, param=[]):
        req_json = ""
        if param[0] in self.set_sub_cmd:
            if param[0] == "node_config" or param[0] == "node_custom_config":
                param_num = len(param)
                file_content = base64.b64encode(param[param_num - 1].encode('utf-8')).decode('utf-8')
                if param_num == 3:
                    req_json = (
                        """
                        {
                            "cmd":"set",
                            "args":["%s", "%s", "%s"]
                        }
                        """ % (param[0], param[1], file_content)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        click.secho("%s" % result_string)
                    else:
                        click.secho("%s" % result_string, fg='red')
                elif param_num == 4:
                    req_json = (
                        """
                        {
                            "cmd":"set",
                            "args":["%s", "%s", "%s", "%s"]
                        }
                        """ % (param[0], param[1], param[3], file_content)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        click.secho("%s" % result_string)
                    else:
                        click.secho("%s" % result_string, fg='red')
                else:
                    click.secho("invalid param num for \"set %s\"!" % param[0], fg='red')
            elif param[0] == "node_config_from_file" or param[0] == "node_custom_config_from_file":
                param_num = len(param)
                metadata = os.stat(param[param_num - 1])
                if metadata.st_size == 0:
                    click.secho("file not exist or empty file!", fg='red')
                    return
                (dirname, filename) = os.path.split(param[param_num - 1])
                with open(param[param_num - 1], 'rb') as f:
                    read_content = f.read()
                    file_content = base64.b64encode(read_content.encode('utf-8')).decode('utf-8')
                    if param_num == 3:
                        req_json = (
                            """
                            {
                                "cmd":"set",
                                "args":["%s", "%s", "%s"]
                            }
                            """ % (param[0], param[1], file_content)
                        )
                        result_string = self.exec_cmd(req_json)
                        result = json.loads(result_string)
                        if result["code"] == 0:
                            click.secho("%s" % result_string)
                        else:
                            click.secho("%s" % result_string, fg='red')
                    elif param_num == 4:
                        req_json = (
                            """
                            {
                                "cmd":"set",
                                "args":["%s", "%s", "%s", "%s"]
                            }
                            """ % (param[0], param[1], param[2], file_content)
                        )
                        result_string = self.exec_cmd(req_json)
                        result = json.loads(result_string)
                        if result["code"] == 0:
                            click.secho("%s" % result_string)
                        else:
                            click.secho("%s" % result_string, fg='red')
                    else:
                        click.secho("invalid param num for \"set %s\"!" % param[0], fg='red')
            elif param[0] == "custom_config":
                param_num = len(param)
                file_content = base64.b64encode(param[param_num - 1].encode('utf-8')).decode('utf-8')
                if param_num == 4:
                    req_json = (
                        """
                        {
                            "cmd":"set",
                            "args":["%s", "%s", "%s", "%s"]
                        }
                        """ % (param[0], param[1], param[2], file_content)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        click.secho("%s" % result_string)
                    else:
                        click.secho("%s" % result_string, fg='red')
                elif param_num == 5:
                    req_json = (
                        """
                        {
                            "cmd":"set",
                            "args":["%s", "%s", "%s", "%s", "%s"]
                        }
                        """ % (param[0], param[1], param[2], param[3], file_content)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        click.secho("%s" % result_string)
                    else:
                        click.secho("%s" % result_string, fg='red')
                elif param_num == 6:
                    req_json = (
                        """
                        {
                            "cmd":"set",
                            "args":["%s", "%s", "%s", "%s", "%s", "%s"]
                        }
                        """ % (param[0], param[1], param[2], param[3], param[4], file_content)
                    )
                    result_string = self.exec_cmd(req_json)
                    result = json.loads(result_string)
                    if result["code"] == 0:
                        click.secho("%s" % result_string)
                    else:
                        click.secho("%s" % result_string, fg='red')
                else:
                    click.secho("invalid param num for \"set %s\"!" % param[0], fg='red')
            elif param[0] == "custom_config_from_file":
                param_num = len(param)
                metadata = os.stat(param[param_num - 1])
                if metadata.st_size == 0:
                    click.secho("file not exist or empty file!", fg='red')
                    return
                (dirname, filename) = os.path.split(param[param_num - 1])
                with open(param[param_num - 1], 'rb') as f:
                    read_content = f.read()
                    file_content = base64.b64encode(read_content.encode('utf-8')).decode('utf-8')
                    if param_num == 3:
                        req_json = (
                            """
                            {
                                "cmd":"set",
                                "args":["%s", "%s", "%s", "%s"]
                            }
                            """ % (param[0], param[1], filename, file_content)
                        )
                        result_string = self.exec_cmd(req_json)
                        result = json.loads(result_string)
                        if result["code"] == 0:
                            click.secho("%s" % result_string)
                        else:
                            click.secho("%s" % result_string, fg='red')
                    elif param_num == 4:
                        req_json = (
                            """
                            {
                                "cmd":"set",
                                "args":["%s", "%s", "%s", "%s", "%s"]
                            }
                            """ % (param[0], param[1], param[2], filename, file_content)
                        )
                        result_string = self.exec_cmd(req_json)
                        result = json.loads(result_string)
                        if result["code"] == 0:
                            click.secho("%s" % result_string)
                        else:
                            click.secho("%s" % result_string, fg='red')
                    elif param_num == 5:
                        req_json = (
                            """
                            {
                                "cmd":"set",
                                "args":["%s", "%s", "%s", "%s", "%s", "%s"]
                            }
                            """ % (param[0], param[1], param[2], param[3], filename, file_content)
                        )
                        result_string = self.exec_cmd(req_json)
                        result = json.loads(result_string)
                        if result["code"] == 0:
                            click.secho("%s" % result_string)
                        else:
                            click.secho("%s" % result_string, fg='red')
                    else:
                        click.secho("invalid param num for \"set %s\"!" % param[0], fg='red')
            else:
                click.secho("invalid param: %s" % param[0], fg='red')
        else:
            click.secho("invalid param: %s" % param[0], fg='red')
                    

@click.group(invoke_without_command=True)
@click.option('--url', '-r', prompt="url",
              help='The url of Beacon server admin.')
@click.pass_context
def cli(ctx, url):
    ctx.obj = Centercli(url)
    if ctx.invoked_subcommand is None:
        while True:
            input_word = click.prompt("nebcli)")
            invoked_cmd = input_word.split(' ')
            if len(invoked_cmd) == 0:
                continue
            if invoked_cmd[0] == "quit" or invoked_cmd[0] == "exit":
                exit(0)
            elif invoked_cmd[0] == "show":
                if len(invoked_cmd) > 1:
                    ctx.obj.show(invoked_cmd[1:])
                else:
                    click.secho("invalid param num for \"show\"!", fg='red')
            elif invoked_cmd[0] == "get":
                if len(invoked_cmd) > 1:
                    ctx.obj.get(invoked_cmd[1:])
                else:
                    click.secho("invalid param num for \"get\"!", fg='red')
            elif invoked_cmd[0] == "set":
                if len(invoked_cmd) > 1:
                    ctx.obj.set(invoked_cmd[1:])
                else:
                    click.secho("invalid param num for \"set\"!", fg='red')
            else:
                click.secho("invalid cmd \"%s\"!" % invoked_cmd[0], fg='red')
    else:
        pass


@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def show(ctx, args):
    """
    \b
    Show usage:
        show target [args]
    \b
    The value of target is as follows:
        ip_white
        subscription
        nodes
        node_report
        node_detail
    \b
    Valid command as follows:
        show ip_white 
        show subscription
        show subscription ${node_type}
        show nodes
        show nodes ${node_type}
        show node_report ${node_type}
        show node_report ${node_type} ${node_identify}
        show node_detail ${node_type}
        show node_detail ${node_type} ${node_identify}
    """
#     print "args",args
    ctx.obj.show(args)

@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def get(ctx, args):
    """
    \b
    Get usage:
        get target [args]
    \b
    The value of target is as follows:
        node_config
        node_custom_config
        custom_config
    \b
    Valid command as follows:
        get node_config ${node_identify}
        get node_custom_config ${node_identify}
        get custom_config ${node_identify} ${config_file_relative_path} ${config_file_name}
    """
    ctx.obj.get(args)

@cli.command()
@click.argument("args", nargs=-1)
@click.pass_context
def set(ctx, args):
    """
    \b
    Set usage:
        set target [args]
    \b
    The value of target is as follows:
        node_config
        node_config_from_file
        node_custom_config
        node_custom_config_from_file
        custom_config
        custom_config_from_file
    \b
    Valid command as follows:
        set node_config ${node_type} ${config_file_content}
        set node_config ${node_type} ${node_identify} ${config_file_content}
        set node_config_from_file ${node_type} ${config_file}
        set node_config_from_file ${node_type} ${node_identify} ${config_file}
        set node_custom_config ${node_type} ${config_content}
        set node_custom_config ${node_type} ${node_identify} ${config_content}
        set node_custom_config_from_file ${node_type} ${config_file}
        set node_custom_config_from_file ${node_type} ${node_identify} ${config_file}
        set custom_config ${node_type} ${config_file_name} ${config_file_content}
        set custom_config ${node_type} ${config_file_relative_path} ${config_file_name} ${config_file_content}
        set custom_config ${node_type} ${node_identify} ${config_file_relative_path} ${config_file_name} ${config_file_content}
        set custom_config_from_file ${node_type} ${config_file}
        set custom_config_from_file ${node_type} ${config_file_relative_path} ${config_file}
        set custom_config_from_file ${node_type} ${node_identify} ${config_file_relative_path} ${config_file}
    """
    ctx.obj.set(args)

if __name__ == '__main__':
    cli()

