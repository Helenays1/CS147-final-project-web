/* eslint-disable max-classes-per-file */
/* eslint-disable no-restricted-globals */
/* eslint-disable no-undef */
$(document).ready(() => {
  // if deployed to a site supporting SSL, use wss://
  const protocol = document.location.protocol.startsWith('https') ? 'wss://' : 'ws://';
  const webSocket = new WebSocket(protocol + location.host);

  // A class for holding the last N points of telemetry for the mointored device
  class DeviceData {
    constructor() {
      this.maxLen = 30;
      this.timeData = new Array(this.maxLen);
      this.accData = new Array(this.maxLen);
      this.gyroData = new Array(this.maxLen);
      this.state = new Array(this.maxLen);
      this.stateColor = new Array(this.maxLen);
      this.detectedCount = 0;
      this.stillDetected = false;
      this.alertCount = 0;
      this.stillAlert = false;
    }

    addData(time, acc, gyro, state) {
      this.timeData.push(time);
      this.accData.push(acc);
      this.gyroData.push(gyro);
      switch (state) {
        case "detected":
          this.state.push(4); // orange
          this.stateColor.push('rgba(255, 165, 0, 0.4)');
          if (!this.stillDetected) {
            this.detectedCount += 1;
            document.getElementById('detectedCount').innerText = this.detectedCount;
            this.stillDetected = true;
          }
          break;
        case "alert":
          this.state.push(8); // red
          this.stateColor.push('rgba(255, 0, 0, 0.4)');
          if (!this.stillAlert) {
            this.stillDetected = false;
            this.alertCount += 1;
            // no need to check for zero division because detect always goes first
            document.getElementById('alertCount').innerText = this.alertCount;
            document.getElementById('alertRate').innerText = (this.alertCount / this.detectedCount).toFixed(2);
            this.stillAlert = true;
          }
          break;
        case "canceled":
          this.state.push(2); // green
          this.stateColor.push('rgba(0, 255, 0, 0.4)');
          document.getElementById('alertCount').innerText = this.alertCount;
          document.getElementById('alertRate').innerText = (this.alertCount / this.detectedCount).toFixed(2);
          break;
        default:
          this.state.push(0);
          this.stateColor.push('rgba(0, 255, 0, 0.4)');
          this.stillDetected = false;
          this.stillAlert = false;
          break;
      }

      if (this.timeData.length > this.maxLen) {
        this.timeData.shift();
        this.accData.shift();
        this.gyroData.shift();
        this.state.shift();
        this.stateColor.shift();
      }
    }
  }

  const monitoredDevice = new DeviceData();

  // Define the chart axes
  const chartData = {
    datasets: [
      {
        fill: false,
        label: 'Acceleration',
        yAxisID: 'Acceleration',
        borderColor: 'rgba(255, 204, 0, 1)',
        pointBoarderColor: 'rgba(255, 204, 0, 1)',
        backgroundColor: 'rgba(255, 204, 0, 0.4)',
        pointHoverBackgroundColor: 'rgba(255, 204, 0, 1)',
        pointHoverBorderColor: 'rgba(255, 204, 0, 1)',
        spanGaps: true,
        type: 'line',
        order: 0,
      },
      {
        fill: false,
        label: 'Gyroscopic',
        yAxisID: 'Gyroscopic',
        borderColor: 'rgba(24, 120, 240, 1)',
        pointBoarderColor: 'rgba(24, 120, 240, 1)',
        backgroundColor: 'rgba(24, 120, 240, 0.4)',
        pointHoverBackgroundColor: 'rgba(24, 120, 240, 1)',
        pointHoverBorderColor: 'rgba(24, 120, 240, 1)',
        spanGaps: true,
        type: 'line',
        order: 1,
      },
      {
        fill: true,
        label: 'State',
        yAxisID: 'Acceleration',
        backgroundColor: 'rgba(0, 255, 0, 0.4)',
        borderColor: 'rgba(0, 255, 0, 1)',
        hoverBackgroundColor: 'rgba(0, 255, 0, 1)',
        hoverBorderColor: 'rgba(0, 100, 0, 1)',
        type: 'bar',
        order: 2
      }
    ]
  };

  chartData.datasets[0].data = monitoredDevice.accData;
  chartData.datasets[1].data = monitoredDevice.gyroData;
  chartData.datasets[2].data = monitoredDevice.state;
  chartData.datasets[2].backgroundColor = monitoredDevice.stateColor;

  const chartOptions = {
    scales: {
      yAxes: [{
        id: 'Acceleration',
        type: 'linear',
        scaleLabel: {
          labelString: 'Acceleration Vector',
          display: true,
        },
        position: 'left',
        ticks: {
          suggestedMin: 0,
          suggestedMax: 8,
          beginAtZero: true
        }
      },
      {
        id: 'Gyroscopic',
        type: 'linear',
        scaleLabel: {
          labelString: 'Gyroscopic Vector',
          display: true,
        },
        position: 'right',
        ticks: {
          suggestedMin: 0,
          suggestedMax: 800,
          beginAtZero: true
        }
      },
            {
        id: 'State',
        type: 'bar',
        scaleLabel: {
          labelString: 'State',
          display: true,
        },
        position: 'left', // unsure if this is necessary
        ticks: {
          suggestedMin: 0,
          suggestedMax: 8,
          beginAtZero: true
        }
      },
    ]
    }
  };

  // Get the context of the canvas element we want to select
  const ctx = document.getElementById('iotChart').getContext('2d');
  const myLineChart = new Chart(
    ctx,
    {
      type: 'bar',
      data: chartData,
      options: chartOptions,
    });

  // Connect chart to monitored device data
  chartData.labels = monitoredDevice.timeData;

 // Unpack message, validate payload, append to monitored device, update chart
  webSocket.onmessage = function onMessage(message) {
    try {
      const messageData = JSON.parse(message.data);
      console.log(messageData);

      if (!messageData.MessageDate || (!messageData.IotData.Acceleration && !messageData.IotData.Gyroscopic && !messageData.IotData.FallState)) {
        return;
      }

      monitoredDevice.addData(messageData.MessageDate, messageData.IotData.Acceleration, messageData.IotData.Gyroscopic, messageData.IotData.FallState);
      myLineChart.update();
    } catch (err) {
      console.error(err);
    }
  };
});
