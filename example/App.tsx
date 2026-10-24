// SPDX-License-Identifier: Apache-2.0 OR MIT
// Streams a pack's answer into a <Text>. Events arrive on the JS thread, so setState is called directly.
//
// In a React Native app that depends on this package (see README): put a signed pack and its .minisig
// into the app's documents folder, set PACK, and run `npx react-native run-android` / `run-ios`.
import React, { useEffect, useRef, useState } from 'react';
import { Button, SafeAreaView, Text, TextInput } from 'react-native';
import { LataiError, Runtime, type RunStream } from 'latai-react-native';

const PACK = 'x4r-l8';

export default function App() {
  const runtime = useRef<Runtime | null>(null);
  const run = useRef<RunStream | null>(null);
  const [text, setText] = useState('hello');
  const [answer, setAnswer] = useState('');
  const [status, setStatus] = useState('starting');

  useEffect(() => {
    Runtime.open()
      .then((rt) => {
        runtime.current = rt;
        setStatus('ready');
      })
      .catch((e: unknown) => setStatus(String(e)));
    return () => {
      void runtime.current?.close();
    };
  }, []);

  async function ask() {
    const rt = runtime.current;
    if (!rt) return;
    setAnswer('');
    setStatus('running');
    try {
      const stream = rt.runStream({ pack: PACK, inputs: { type: 'text', text }, output: { max_tokens: 64 } });
      run.current = stream;
      for await (const e of stream) {
        if (e.event === 'delta') setAnswer((a) => a + e.text);
        else if (e.event === 'final') setStatus(`done (${e.response.finish})`);
        else setStatus(`${e.error.code}: ${e.error.message}`);
      }
    } catch (e) {
      setStatus(e instanceof LataiError ? `${e.code} ${e.message}` : String(e));
    }
  }

  return (
    <SafeAreaView>
      <TextInput value={text} onChangeText={setText} />
      <Button title="Ask" onPress={() => void ask()} />
      <Button title="Stop" onPress={() => run.current?.cancel()} />
      <Text>{answer}</Text>
      <Text>{status}</Text>
    </SafeAreaView>
  );
}
