import threading
import psycopg2
import paho.mqtt.client as mqtt
import uuid
import json
import time

# Configuración base de datos
db_config = {
    "host": "localhost",
    "database": "Prueba2",
    "user": "postgres",
    "password": "jumilla",
    "port": 5432
}

# Variable global para marcar cuándo hay que actualizar
actualizar_tablas = False
lock = threading.Lock()  # Para sincronizar acceso a la variable

# Función para registrar el pedido
def registrar_pedido(conn, dni, nombre, color, talla, piso, fila, columna):
    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO cliente (dni, nombreyapellidos)
            VALUES (%s, %s)
            ON CONFLICT (dni) DO NOTHING;
        """, (dni, nombre))

        id_pedido = str(uuid.uuid4())

        cur.execute("""
            INSERT INTO pedido (id_pedido, color, talla, dni_cliente)
            VALUES (%s, %s, %s, %s);
        """, (id_pedido, color, talla, dni))

        cur.execute("""
            INSERT INTO almacen (id_pedido, piso, fila, columna)
            VALUES (%s, %s, %s, %s);
        """, (id_pedido, piso, fila, columna))

        conn.commit()

    print(f" Pedido registrado: {id_pedido}")
    # Marcar para actualizar
    with lock:
        global actualizar_tablas
        actualizar_tablas = True

# Callback MQTT
def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        talla = payload["talla"]
        color = payload["color"]
        dni = payload["dni"]
        nombre = payload["nombre"]

        print(f" Mensaje recibido: {payload}")
        conn = psycopg2.connect(**db_config)
        registrar_pedido(conn, dni, nombre, color, talla, 0, 0, 0)
        conn.close()

    except Exception as e:
        print(f" Error al procesar mensaje: {e}")

# Función que chequea si hay que hacer SELECT * y mostrar
def monitor_actualizaciones():
    global actualizar_tablas
    while True:
        with lock:
            if actualizar_tablas:
                try:
                    conn = psycopg2.connect(**db_config)
                    cur = conn.cursor()

                    print("\n Tabla cliente:")
                    cur.execute("SELECT * FROM cliente")
                    for row in cur.fetchall():
                        print(row)

                    print("\n Tabla pedido:")
                    cur.execute("SELECT * FROM pedido")
                    for row in cur.fetchall():
                        print(row)

                    print("\n Tabla almacen:")
                    cur.execute("SELECT * FROM almacen")
                    for row in cur.fetchall():
                        print(row)

                    conn.close()
                except Exception as e:
                    print(f" Error al hacer SELECT: {e}")
                finally:
                    actualizar_tablas = False
        time.sleep(1)

# Lanzar hilo monitor
threading.Thread(target=monitor_actualizaciones, daemon=True).start()

# MQTT
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, transport="websockets")
client.tls_set()
client.on_message = on_message
client.connect("broker.emqx.io", 8084)
client.subscribe("tienda/pedidos")

print(" Esperando pedidos...")
client.loop_forever()
