# ServerConnector


## Instalation with systemd

1. Create `/etc/systemd/system/minecraft.service`
2. Fill it with:

       [Unit]
       Description=Minecraft
       After=network.target

       [Service]
       Type=simple
       User=fernando
       ExecStart=/home/fernando/serverconnector /home/fernando/mc/server.ini
       Restart=on-failure
       KillMode=mixed

       [Install]
       WantedBy=multi-user.target

