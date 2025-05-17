const paywall = document.getElementById("paywall");
const iframe = document.getElementById("browser");

// Abrir URL no iframe
document.getElementById("goButton").onclick = () => {
  const url = document.getElementById("urlInput").value;
  iframe.src = url;
};

// Conectar ao MQTT Broker (HiveMQ público para teste)
const client = mqtt.connect("wss://broker.hivemq.com:8884/mqtt");

client.on("connect", () => {
  console.log("Conectado ao MQTT");
  client.subscribe("volume/infraction");
  client.subscribe("volume/clear");
});

client.on("message", (topic, message) => {
  if (topic === "volume/infraction") {
    paywall.classList.add("locked");
  } else if (topic === "volume/clear") {
    paywall.classList.remove("locked");
  }
});

client.on("error", (err) => {
  console.error("Erro no MQTT:", err);
});
