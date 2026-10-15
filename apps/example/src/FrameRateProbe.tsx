import React, { useEffect, useMemo, useRef, useState } from "react";
import { PixelRatio, StyleSheet, Switch, Text, View } from "react-native";
import type { CanvasRef, RNCanvasContext } from "react-native-webgpu";
import { Canvas, useDevice } from "react-native-webgpu";
import type { SharedValue } from "react-native-reanimated";
import { useSharedValue } from "react-native-reanimated";
import {
  createWorkletRuntime,
  runOnRuntime,
  scheduleOnRN,
  scheduleOnUI,
} from "react-native-worklets";

type Driver = "js" | "ui" | "dedicated";
type CanvasSize = "full" | "small";

const renderLoop = (
  running: SharedValue<boolean>,
  device: GPUDevice,
  context: RNCanvasContext,
  report: (framesPerSecond: number) => void,
  onWorklet: boolean,
) => {
  "worklet";
  let frame = 0;
  let windowStart = Date.now();
  let framesInWindow = 0;
  const render = () => {
    const hues = [
      [1, 0, 0, 1],
      [1, 0.5, 0, 1],
      [1, 1, 0, 1],
      [0, 1, 0, 1],
      [0, 1, 1, 1],
      [0, 0, 1, 1],
      [0.5, 0, 1, 1],
      [1, 0, 1, 1],
    ];
    const hue = hues[frame++ % hues.length];
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [
        {
          view: context.getCurrentTexture().createView(),
          clearValue: hue,
          loadOp: "clear",
          storeOp: "store",
        },
      ],
    });
    pass.end();
    device.queue.submit([encoder.finish()]);
    context.present();
    framesInWindow++;
    const now = Date.now();
    if (now - windowStart >= 1000) {
      const framesPerSecond = Math.round(
        (framesInWindow * 1000) / (now - windowStart),
      );
      if (onWorklet) {
        scheduleOnRN(report, framesPerSecond);
      } else {
        report(framesPerSecond);
      }
      framesInWindow = 0;
      windowStart = now;
    }
    if (running.value) {
      requestAnimationFrame(render);
    }
  };
  requestAnimationFrame(render);
};

function FrameRateProbeScreen({
  driver,
  size,
}: {
  driver: Driver;
  size: CanvasSize;
}) {
  const { device } = useDevice();
  const ref = useRef<CanvasRef>(null);
  const running = useSharedValue(true);
  const [animating, setAnimating] = useState(true);
  const [textureView, setTextureView] = useState(false);
  const [framesPerSecond, setFramesPerSecond] = useState(0);
  const dedicatedRuntime = useMemo(
    () =>
      driver === "dedicated"
        ? createWorkletRuntime({ name: "FrameRateProbeRuntime" })
        : null,
    [driver],
  );

  useEffect(() => {
    const context = ref.current?.getContext("webgpu");
    if (!context || !device) {
      return;
    }
    const canvas = context.canvas as HTMLCanvasElement;
    canvas.width = canvas.clientWidth * PixelRatio.get();
    canvas.height = canvas.clientHeight * PixelRatio.get();
    context.configure({
      device,
      format: navigator.gpu.getPreferredCanvasFormat(),
      alphaMode: "opaque",
    });
    running.value = animating;
    if (!animating) {
      return;
    }
    if (driver === "ui") {
      scheduleOnUI(
        renderLoop,
        running,
        device,
        context,
        setFramesPerSecond,
        true,
      );
    } else if (driver === "dedicated" && dedicatedRuntime !== null) {
      runOnRuntime(dedicatedRuntime, renderLoop)(
        running,
        device,
        context,
        setFramesPerSecond,
        true,
      );
    } else {
      renderLoop(running, device, context, setFramesPerSecond, false);
    }
    return () => {
      running.value = false;
    };
  }, [device, animating, driver, dedicatedRuntime, running]);

  return (
    <View style={styles.container}>
      <View style={styles.stage}>
        <Canvas
          ref={ref}
          style={size === "full" ? StyleSheet.absoluteFill : styles.small}
          android={{ surfaceType: textureView ? "TextureView" : "SurfaceView" }}
        />
      </View>
      <View style={styles.panel}>
        <Text style={styles.readout}>
          {animating ? `${framesPerSecond} fps` : "static"}
        </Text>
        <Text style={styles.label}>{`driver: ${driver}, ${size}`}</Text>
        <View style={styles.row}>
          <Text style={styles.label}>animate</Text>
          <Switch value={animating} onValueChange={setAnimating} />
        </View>
        <View style={styles.row}>
          <Text style={styles.label}>TextureView</Text>
          <Switch value={textureView} onValueChange={setTextureView} />
        </View>
      </View>
    </View>
  );
}

export const FrameRateProbe = () => (
  <FrameRateProbeScreen driver="js" size="full" />
);
export const FrameRateProbeUIThread = () => (
  <FrameRateProbeScreen driver="ui" size="full" />
);
export const FrameRateProbeDedicatedThread = () => (
  <FrameRateProbeScreen driver="dedicated" size="full" />
);
export const FrameRateProbeSmall = () => (
  <FrameRateProbeScreen driver="js" size="small" />
);
export const FrameRateProbeSmallUIThread = () => (
  <FrameRateProbeScreen driver="ui" size="small" />
);

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: "white",
  },
  stage: {
    ...StyleSheet.absoluteFillObject,
    justifyContent: "center",
    alignItems: "center",
  },
  small: {
    width: 260,
    height: 200,
  },
  panel: {
    position: "absolute",
    top: 24,
    left: 24,
    padding: 16,
    borderRadius: 12,
    backgroundColor: "white",
  },
  readout: {
    fontSize: 40,
    fontWeight: "700",
    color: "black",
  },
  row: {
    flexDirection: "row",
    alignItems: "center",
    marginTop: 8,
  },
  label: {
    fontSize: 20,
    color: "black",
    marginRight: 12,
  },
});
