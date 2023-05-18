;; TBatSave host MNL bridge template.
;; Rendered at runtime into var/runtime to keep Tianzheng-side edits minimal.

(defun t3conv:tbx-log (msg / lf)
  (setq lf (open "__TBX_TRIGGER_LOG__" "a"))
  (if lf
    (progn
      (write-line (strcat (rtos (getvar "date") 2 6) " " msg) lf)
      (close lf)
    )
  )
  (princ)
)

(defun t3conv:tbx-host-loader ()
  (princ)
  (cond
    ((findfile "__TBX_HOST_BOOTSTRAP__")
      (setq t3conv:tbx-host-runtime-loaded-in-session T)
      (t3conv:tbx-log "TBatSave host bootstrap trigger found via tangent.mnl")
      (load "__TBX_HOST_RUNTIME_SCRIPT__" nil))
    ((and (findfile "__TBX_HOST_READY__")
          (not t3conv:tbx-host-runtime-loaded-in-session))
      (setq t3conv:tbx-host-runtime-loaded-in-session T)
      (t3conv:tbx-log "TBatSave host ready trigger found via tangent.mnl")
      (load "__TBX_HOST_RUNTIME_SCRIPT__" nil))
    ((findfile "__TBX_HOST_READY__")
      (t3conv:tbx-log "TBatSave host ready marker already present; runtime load skipped"))
  )
  (princ)
)

(if (not t3conv:tbx-host-loader-registered)
  (progn
    (setq t3conv:tbx-host-loader-registered T)
    (setq S::STARTUP (append S::STARTUP '((t3conv:tbx-host-loader))))
    (t3conv:tbx-log "TBatSave tangent.mnl bridge registered")
  )
)

(if (findfile "__TBX_HOST_BOOTSTRAP__")
  (progn
    (t3conv:tbx-log "TBatSave tangent.mnl bridge immediate trigger")
    (t3conv:tbx-host-loader)
  )
)

(princ)
