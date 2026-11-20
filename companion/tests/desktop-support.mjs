export async function bounded(action, milliseconds, label) {
  let timer;
  try {
    return await Promise.race([action(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${label} timed out after ${milliseconds} ms`)), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

export async function closeDesktop(application, milliseconds = 10000) {
  try { await bounded(() => application.close(), milliseconds, 'Electron shutdown'); }
  catch (error) {
    application.process().kill('SIGKILL');
    throw error;
  }
}
