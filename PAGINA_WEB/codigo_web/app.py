from flask import Flask, request, render_template
import paho.mqtt.client as mqtt
import ssl
import qrcode
import io
import base64
import os
import json

app = Flask(__name__)

# MQTT Config
MQTT_BROKER = 'broker.emqx.io'
MQTT_PORT   = 8084  # WebSocket seguro
MQTT_TOPIC  = 'tienda/pedidos'

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/enviar', methods=['POST'])
def enviar():
    try:
        # 1) Recuperamos talla y color desde el formulario
        talla = request.form.get('talla', '').strip()
        color = request.form.get('color', '').strip()
        if not talla or not color:
            return "Faltan los campos talla o color", 400

        # 2) Creamos el JSON con ambos campos
        payload = {
            "talla": talla,
            "color": color
        }
        mensaje_json = json.dumps(payload)

        # 3) Configuramos MQTT y publicamos el JSON
        mqtt_client = mqtt.Client(transport="websockets")
        mqtt_client.tls_set(cert_reqs=ssl.CERT_NONE)
        mqtt_client.tls_insecure_set(True)

        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                print("Conectado correctamente, publicando JSON…")
                client.publish(MQTT_TOPIC, mensaje_json)
            else:
                print(f"Falló la conexión MQTT: {rc}")

        mqtt_client.on_connect = on_connect
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_start()

        import time
        time.sleep(2)
        mqtt_client.loop_stop()

        # 4) Generamos el QR a partir del JSON
        qr = qrcode.make(mensaje_json)
        buffer = io.BytesIO()
        qr.save(buffer, format="PNG")
        img_str = base64.b64encode(buffer.getvalue()).decode("utf-8")

        # 5) Devolvemos la plantilla con la imagen
        return render_template('qr.html', qr_image=img_str)

    except Exception as e:
        return f"Error: {e}", 500

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5000))
    app.run(host='0.0.0.0', port=port, debug=True)
