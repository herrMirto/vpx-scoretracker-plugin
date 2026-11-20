import test from 'node:test';
import assert from 'node:assert/strict';
import { readScanStream } from '../src/scan-stream.ts';
const snapshot = {generatedAt:'2026-10-11',tablesRoot:'/Tables',sourcesScanned:1,vpxFilesFound:1,games:[],warnings:[{source:'São Paulo/scores.json',message:'test'}]};
function chunked(text: string): Response {
  const bytes = new TextEncoder().encode(text);
  return new Response(new ReadableStream({start(controller) {
    for (let index=0; index<bytes.length; index+=7) controller.enqueue(bytes.slice(index,index+7));
    controller.close();
  }}));
}
test('score stream handles split UTF-8 and JSON and distinguishes partial from complete',async () => {
  const partial = {...snapshot,sourcesScanned:0};
  const seen:unknown[]=[];
  const result=await readScanStream(chunked(JSON.stringify({snapshot:partial,complete:false})+'\n'+JSON.stringify({snapshot,complete:true})+'\n'),value=>seen.push(value));
  assert.deepEqual(seen,[partial]); assert.deepEqual(result,snapshot);
});
test('interrupted score stream cannot masquerade as a completed scan',async () => {
  await assert.rejects(readScanStream(chunked(JSON.stringify({snapshot,complete:false})+'\n'),()=>{}),/interrupted/);
});
test('score stream surfaces filesystem errors',async () => {
  await assert.rejects(readScanStream(chunked(JSON.stringify({error:'Tables folder unavailable'})+'\n'),()=>{}),/Tables folder unavailable/);
});
