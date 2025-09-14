import http from 'k6/http';
import { check } from 'k6';
export let options = { vus: 128, duration: '2m' };
export default function () {
  let res = http.get('http://127.0.0.1:8080/io?size=8388608');
  check(res, { 'status 200': (r) => r.status === 200 });
}
