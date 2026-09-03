/**
 * WHATWG Encoding, implemented natively.
 *
 * Both classes are installed as globals by the QuickJS module; these
 * declarations exist so TypeScript agrees, and so the package has an entry
 * point to import for its side effect.
 */

export declare class TextEncoder {
  readonly encoding: 'utf-8';
  encode(input?: string): Uint8Array;
  encodeInto(source: string, destination: Uint8Array): { read: number; written: number };
}

export declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: 'utf-8';
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView): string;
}
