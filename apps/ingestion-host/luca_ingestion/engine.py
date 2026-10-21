"""Versioned local process boundary to the C++ financial engine, never a shell."""
import hashlib
import os
from pathlib import Path
import select
import subprocess
import time

from .store import IntakeError


class Engine:
    def __init__(self, executable, timeout=30):
        path=Path(executable).resolve(strict=True)
        self.digest=hashlib.sha256(path.read_bytes()).hexdigest()
        self.process=subprocess.Popen([str(path)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL,bufsize=0)
        self.buffer=bytearray(); self.timeout=timeout
        self.sequence=0; self.revision=0; self.recorded_ns=0
        try:
            if self._line()!=["LUCA_INGESTION","1","luca/0.1"]:
                raise IntakeError("ENGINE_VERSION_MISMATCH")
        except BaseException:
            self.close(); raise

    def _line(self):
        end=time.monotonic()+self.timeout
        while b"\n" not in self.buffer:
            wait=max(0,end-time.monotonic())
            if not select.select([self.process.stdout],[],[],wait)[0]:
                raise IntakeError("ENGINE_TIMEOUT",retryable=True)
            data=os.read(self.process.stdout.fileno(),65536)
            if not data: raise IntakeError("ENGINE_UNAVAILABLE",retryable=True)
            self.buffer.extend(data)
            if len(self.buffer)>4*1024*1024: raise IntakeError("ENGINE_PROTOCOL")
        line,_,rest=self.buffer.partition(b"\n"); self.buffer=bytearray(rest)
        return line.decode("ascii").split("\t")

    def _send(self, command, lines=()):
        data=(command+"\n"+"".join(line+"\n" for line in lines)).encode("ascii")
        try:
            while data:
                n=self.process.stdin.write(data)
                if not n: raise BrokenPipeError()
                data=data[n:]
        except (BrokenPipeError,OSError): raise IntakeError("ENGINE_UNAVAILABLE",retryable=True) from None

    @staticmethod
    def _rows(packets, recorded_ns=0):
        for packet in packets:
            values=list(packet); values[6]=str(recorded_ns)
            yield "\t".join(value.encode("utf-8").hex() for value in values)

    def validate(self, packets):
        self._send(f"V\t{len(packets)}",self._rows(packets))
        result=[]
        for _ in packets:
            row=self._line()
            if row==["OK"]: result.append(None)
            elif len(row)==4 and row[0]=="INVALID":
                result.append({"code":row[1],"field_path":bytes.fromhex(row[2]).decode(),
                               "message":bytes.fromhex(row[3]).decode()})
            else: raise IntakeError("ENGINE_PROTOCOL")
        return result

    def sync(self, db):
        state=dict(db.execute("SELECT * FROM ledger_state WHERE id=1").fetchone())
        if self.sequence>state["last_sequence"]: raise IntakeError("LEDGER_CORRUPT")
        cursor=db.execute("SELECT * FROM ledger_records WHERE sequence>? ORDER BY sequence",(self.sequence,))
        while rows:=cursor.fetchmany(1000):
            for index,row in enumerate(rows):
                if row["sequence"]!=self.sequence+index+1 or hashlib.sha256(row["canonical"]).hexdigest()!=row["digest"]:
                    raise IntakeError("LEDGER_CORRUPT")
            self._send(f"R\t{len(rows)}",(row["canonical"].hex() for row in rows))
            if self._line()!=["OK"]: raise IntakeError("ENGINE_PROTOCOL")
            self.sequence+=len(rows)
        if self.sequence!=state["last_sequence"]: raise IntakeError("LEDGER_CORRUPT")
        self.revision=state["revision"]; self.recorded_ns=state["recorded_ns"]
        return state

    def prepare(self, packets, recorded_ns):
        self._send(f"P\t{len(packets)}",self._rows(packets,recorded_ns))
        row=self._line()
        if len(row)==5 and row[0]=="ERROR":
            return None,{"category":row[1],"canonical_id":bytes.fromhex(row[2]).decode(),
                         "related_id":bytes.fromhex(row[3]).decode(),"message":bytes.fromhex(row[4]).decode()}
        if row!=["READY",str(len(packets))]: raise IntakeError("ENGINE_PROTOCOL")
        accepted=[]
        for index in range(len(packets)):
            data=self._line()
            if len(data)!=2 or int(data[0])!=self.sequence+index+1: raise IntakeError("ENGINE_PROTOCOL")
            accepted.append((int(data[0]),bytes.fromhex(data[1])))
        return accepted,None

    def discard(self):
        self._send("X")
        if self._line()!=["OK"]: raise IntakeError("ENGINE_PROTOCOL")

    def publish(self, count):
        self._send("C")
        if self._line()!=["OK"]: raise IntakeError("ENGINE_PROTOCOL")
        self.sequence+=count; self.revision+=1

    def close(self):
        if self.process.stdin: self.process.stdin.close()
        try: self.process.wait(timeout=2)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        if self.process.stdout: self.process.stdout.close()
