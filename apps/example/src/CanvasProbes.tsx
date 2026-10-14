import React, { useEffect, useRef, useState } from "react";
import { PixelRatio, Text, View } from "react-native";
import type { CanvasRef } from "react-native-webgpu";
import { Canvas, useDevice } from "react-native-webgpu";

// Evidence probes for the getContext() sizing and ref handle fixes; never part
// of the PR.

const CANVAS_SIZE = 240;

const checkerWGSL = /* wgsl */ `
@vertex fn vs(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  var corners = array<vec2f, 3>(vec2f(-1, -3), vec2f(3, 1), vec2f(-1, 1));
  return vec4f(corners[index], 0, 1);
}

@fragment fn fs(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let cell = vec2u(position.xy) / 3u;
  let dark = (cell.x + cell.y) % 2u == 0u;
  return select(vec4f(1, 1, 1, 1), vec4f(0.1, 0.1, 0.1, 1), dark);
}`;

// The docs' setup run once while the canvas enters at half its size, as in a
// zoom-in transition, then a checkerboard of 3-pixel cells drawn every frame
// after the transition ends.
export const CanvasLayoutProbe = () => {
  const ref = useRef<CanvasRef>(null);
  const { device } = useDevice();
  const [scale, setScale] = useState(0.5);
  const [bufferSize, setBufferSize] = useState("");
  useEffect(() => {
    const context = ref.current?.getContext("webgpu");
    if (!device || !context) {
      return;
    }
    const canvas = context.canvas as HTMLCanvasElement;
    canvas.width = canvas.clientWidth * PixelRatio.get();
    canvas.height = canvas.clientHeight * PixelRatio.get();
    const format = navigator.gpu.getPreferredCanvasFormat();
    context.configure({ device, format, alphaMode: "premultiplied" });
    const module = device.createShaderModule({ code: checkerWGSL });
    const pipeline = device.createRenderPipeline({
      layout: "auto",
      vertex: { module, entryPoint: "vs" },
      fragment: { module, entryPoint: "fs", targets: [{ format }] },
    });
    setBufferSize(`${canvas.width} × ${canvas.height}`);
    setScale(1);
    let frame = 0;
    const draw = () => {
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [
          {
            view: context.getCurrentTexture().createView(),
            loadOp: "clear",
            storeOp: "store",
            clearValue: [1, 0, 0, 1],
          },
        ],
      });
      pass.setPipeline(pipeline);
      pass.draw(3);
      pass.end();
      device.queue.submit([encoder.finish()]);
      context.present();
      frame = requestAnimationFrame(draw);
    };
    frame = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(frame);
  }, [device]);
  return (
    <View
      style={{
        flex: 1,
        alignItems: "center",
        justifyContent: "center",
        backgroundColor: "white",
      }}
    >
      <Canvas
        ref={ref}
        opaque={false}
        style={{
          width: CANVAS_SIZE,
          height: CANVAS_SIZE,
          transform: [{ scale }],
        }}
      />
      <Text style={{ color: "black", marginTop: 16 }}>
        {`drawing buffer ${bufferSize} for a ${CANVAS_SIZE} × ${CANVAS_SIZE} pt canvas at ${PixelRatio.get()}×`}
      </Text>
    </View>
  );
};

// A canvas whose ref is a state setter, the shape <Canvas ref={setCanvas} />.
export const CanvasHandleProbe = () => {
  const [canvas, setCanvas] = useState<CanvasRef | null>(null);
  const renders = useRef(0);
  renders.current += 1;
  return (
    <View
      style={{
        flex: 1,
        alignItems: "center",
        justifyContent: "center",
        backgroundColor: "white",
      }}
    >
      <Canvas
        ref={setCanvas}
        style={{ width: CANVAS_SIZE, height: CANVAS_SIZE / 2 }}
      />
      <Text style={{ color: "black", marginTop: 16 }}>
        {`<Canvas ref={setCanvas} />: ${renders.current} renders, handle ${canvas === null ? "not set" : "set"}`}
      </Text>
    </View>
  );
};
