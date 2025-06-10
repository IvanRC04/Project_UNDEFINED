// script.js

document.addEventListener('DOMContentLoaded', () => {
  // 1) Mapea cada color con su fichero .glb
  const modelMap = {
    Rojo: 'Crocs-Rojo.glb',
    Azul: 'Crocs-Azul.glb',
    Verde: 'Crocs-Verde.glb',
    Negro: 'Crocs-Negro.glb',
    Blanco: 'Crocs-Blanco.glb',
    Amarillo: 'Crocs-Amarillo.glb',
    Morado: 'Crocs-Morado.glb',
    Rosa: 'Crocs-Rosa.glb',
  };

  // Ruta base de modelos
  const modelFolder = '../static/images/';
  const colorSelect = document.getElementById('color');
  const modelViewer = document.getElementById('croc-viewer');

  // Función para actualizar el modelo
  function updateModel() {
    const color = colorSelect.value;
    if (!color || !modelMap[color]) return;
    const newSrc = modelFolder + modelMap[color];
    console.log('Cambiando modelo a:', newSrc);
    modelViewer.setAttribute('src', newSrc);
  }

  // Listener para cambio de color
  colorSelect.addEventListener('change', updateModel);

 // 2) Conexión MQTT
  console.log('Intentando conectar a MQTT...');
  const client = mqtt.connect('wss://broker.emqx.io:8084/mqtt');
  client.on('connect', () => console.log('✅ Conectado a MQTT por WSS'));
  client.on('error', err => console.error('❌ Error MQTT:', err));

  // 3) Enviar mensaje al pulsar botón
  sendBtn.addEventListener('click', () => {
    const talla = document.getElementById('talla').value;
    const color = colorSelect.value;
    const nombre = document.getElementById('nombre').value;
    const dni    = document.getElementById('dni').value;

    if (!talla || !color || !nombre || !dni) {
      alert('Rellena todos los campos antes de enviar.');
      return;
    }

    const payload = JSON.stringify({ nombre, dni, talla, color });
    console.log('Publicando:', payload);

    if (client.connected) {
      client.publish('tienda/pedidos', payload, {}, err => {
        if (err) {
          console.error('Error al publicar:', err);
        } else {
          alert('Pedido enviado!');
        }
      });
    } else {
      alert('No conectado a MQTT aún.');
    }
  });
});

  // === QR COMPRA / DEVOLUCIÓN ===
  const btnCompra = document.getElementById('btnCompra');
  const btnDevolucion = document.getElementById('btnDevolucion');
  const qrCompra = document.getElementById('qrCompra');
  const qrDevolucion = document.getElementById('qrDevolucion');

  if (btnCompra && qrCompra && btnDevolucion && qrDevolucion) {
    btnCompra.addEventListener('click', () => {
      qrCompra.src = '../static/images/venta.png';
      qrCompra.style.display = 'block';
      qrDevolucion.style.display = 'none';
    });

    btnDevolucion.addEventListener('click', () => {
      qrDevolucion.src = '../static/images/devolucion.png';
      qrDevolucion.style.display = 'block';
      qrCompra.style.display = 'none';
    });
  };