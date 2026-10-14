import React, { useEffect, useRef, useState } from "react";
import { Button, ScrollView, StyleSheet, Text, View } from "react-native";
import type { CanvasRef, RNCanvasContext } from "react-native-webgpu";
import { Canvas } from "react-native-webgpu";

import {
  diagnosticStyles,
  drawClearFrame,
  initGPU,
  useDiagnosticLog,
} from "./surfaceLifecycle";

interface CanvasGPU {
  device: GPUDevice;
  context: RNCanvasContext;
}

export const SurfaceFrameRate = () => {
  const ref = useRef<CanvasRef>(null);
  const { log, append } = useDiagnosticLog();
  const [gpu, setGPU] = useState<CanvasGPU | null>(null);
  const [animating, setAnimating] = useState(true);
  const [framesPerSecond, setFramesPerSecond] = useState(0);

  useEffect(() => {
    let cancelled = false;
    initGPU(append)
      .then(({ device, format }) => {
        if (cancelled) {
          return;
        }
        const context = ref.current?.getContext("webgpu");
        if (!context) {
          append("the canvas has no webgpu context");
          return;
        }
        context.configure({ device, format, alphaMode: "opaque" });
        setGPU({ device, context });
      })
      .catch((error: Error) => append(`init failed: ${error.message}`));
    return () => {
      cancelled = true;
    };
  }, [append]);

  useEffect(() => {
    if (!gpu || !animating) {
      return;
    }
    let frame = 0;
    let readoutStart = performance.now();
    let framesSinceReadout = 0;
    let animationFrame = 0;
    const render = () => {
      try {
        drawClearFrame(gpu.device, gpu.context, frame++);
      } catch (e) {
        append(`frame threw: ${(e as Error).message}`);
        return;
      }
      framesSinceReadout++;
      const now = performance.now();
      if (now - readoutStart >= 1000) {
        setFramesPerSecond(
          Math.round((framesSinceReadout * 1000) / (now - readoutStart)),
        );
        framesSinceReadout = 0;
        readoutStart = now;
      }
      animationFrame = requestAnimationFrame(render);
    };
    animationFrame = requestAnimationFrame(render);
    return () => cancelAnimationFrame(animationFrame);
  }, [append, gpu, animating]);

  return (
    <View style={diagnosticStyles.container}>
      <View style={diagnosticStyles.controls}>
        <Text style={diagnosticStyles.description}>
          A 260 x 200 opaque canvas, a SurfaceView on Android, redrawn on every
          animation frame. On a display that adapts its refresh rate, it should
          run at the highest rate while it animates.
        </Text>
        <Button
          testID="toggle-animate"
          title={`animate: ${animating}`}
          onPress={() => setAnimating((value) => !value)}
        />
        <Text testID="frame-rate" style={styles.readout}>
          {animating ? `${framesPerSecond} fps` : "static"}
        </Text>
      </View>
      <View style={styles.stage}>
        <Canvas ref={ref} style={styles.canvas} />
      </View>
      <ScrollView style={diagnosticStyles.log}>
        {log.map((line, i) => (
          <Text key={i} style={diagnosticStyles.logLine}>
            {line}
          </Text>
        ))}
      </ScrollView>
    </View>
  );
};

const styles = StyleSheet.create({
  readout: { fontSize: 32, fontWeight: "bold" },
  stage: { flex: 1, alignItems: "center", justifyContent: "center" },
  canvas: { width: 260, height: 200 },
});
