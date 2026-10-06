# RemoteOps — IT24103080

## Personalisation Values
- Agent Port: 9410
- SID: 0803
- Auth Token: OPS-3080
- Log File: remoteops_IT24103080.log
- Storage Path: ./agentfiles/IT24103080/
- Source Files: agent_080.c, controller_080.c, Makefile_080

## Build
    make -f Makefile_080

## Run
Terminal 1 (Agent):
    ./agent_080

Terminal 2 (Controller):
    ./controller_080

## Commands
    AUTH OPS-3080
    SYSINFO
    LISTPROC
    EXEC DATE | UPTIME | DISKFREE | HOSTNAME | WHOAMI
    PUT <filename> <filesize>
    GET <filename>
    MONITOR START <udp_port>
    MONITOR STOP
    QUIT

## Author
Fernando L.A.S — IT24103080
