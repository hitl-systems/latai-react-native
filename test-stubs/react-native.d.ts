// SPDX-License-Identifier: Apache-2.0 OR MIT
// TEST AID ONLY: just enough of 'react-native' to type-check this package without installing
// React Native (tsconfig.typecheck.json maps the module here). Not shipped, not used at run time.
declare module 'react-native' {
  export const NativeModules: Record<string, any>;
  export const Platform: { OS: 'ios' | 'android' | string };
  export const View: any;
  export const Text: any;
  export const Button: any;
  export const TextInput: any;
  export const SafeAreaView: any;
}
declare module 'react' {
  export function useState<T>(init: T): [T, (v: T | ((p: T) => T)) => void];
  export function useEffect(f: () => void | (() => void), deps?: unknown[]): void;
  export function useRef<T>(v: T): { current: T };
  const React: any;
  export default React;
}
declare namespace JSX {
  interface IntrinsicElements { [name: string]: any }
}
